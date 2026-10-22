# DB Async Entry Points (coroutine-resume style)

> Status: **implemented (M1–M4 all landed)**. This document is the design basis of the DB async path;
> milestone status at the end; [DB Usage Discipline](db-discipline.md) has completed the downgrade planned
> in the "Compatibility and migration" section (rules 1/2 → recommendations, 3–6 still hard). The
> authoritative Lua-side usage contract is the Database (SQL) section of the Lua API. Source:
> the [architecture review](architecture-review.md) §4 [HIGH] risk conclusion —
> `shield.database.v1` is fully synchronous and blocking; a slow query wedges the entire VM of the
> initiating service; this is step (b) of the review's two-step recommendation.

## Problem

Each service is a single-threaded actor + exclusive Lua VM. The current DB call chain **enters the driver
synchronously on the actor thread**:

```text
Lua db:query(sql, params)
  → 插件 register_lua 注册的 C closure（plugins/*/shield_db_*.cpp）
  → 直接调用原生驱动（mysql_real_query / PQexec / sqlite3_step），阻塞
  → 期间该 service 的所有消息、RPC、timer 回调全部排队
```

Key facts (design inputs, all verified):

1. **The Lua side bypasses the C vtable entirely**. The proxy methods of `shield.database.<driver>` are
   C closures registered in the plugin's `register_lua`, entering the driver directly;
   `shield_database_v1` (include/shield/plugin/database.h) has **no callers at all** in-tree. So "adding
   callback parameters to the vtable" cannot save the Lua side.
2. The blocking point is the **calling service's actor thread**; connection pool waiting
   (`acquire_timeout_ms`, default 10s) happens on the same thread, blocking longer when the pool is
   exhausted.
3. The host already has a complete, production-proven **coroutine suspend/resume machine**
   (fully reused by `shield.call`):
   - `LuaServiceManager::suspend_for_call` (lua_service.cpp): anchors the caller coroutine, registers the
     pending call, and drives timeouts with a CAF delayed message;
   - `resume_caller` / `complete_call`: callable from **any thread**; re-queued via the caller actor's
     mailbox; the yield-window race (driving-phase guard) is already handled;
   - the Lua-side shim pattern: the C++ primitive returns a session id, the Lua wrapper does
     `coroutine.yield()`, and on completion resumes exactly once with the JSON result.
4. `host_api.lua_post_to_service` (host_api.h) already provides the delivery primitive from "plugin thread
   → service actor scheduling point" (destroy_fn exactly-once semantics); the queue.redis consumer thread
   already uses the same pattern.

## Decision

**Adopt coroutine-resume async entry points**: proxy methods like `db:query`, inside coroutine dispatch,
**suspend the caller's coroutine** (without blocking the actor); the SQL executes on the **plugin's own
worker thread pool**; completion resumes the coroutine via the host resume primitive and delivers results
in the existing return shapes. The `shield.call` suspend/resume machine is reused end to end; no new
scheduling is invented.

```text
Lua db:query(sql, params)                （协程 dispatch 内）
  → 插件 Lua shim：调 __db_submit(...)   （插件自带 lua 文件 + C closure）
  → C closure：lua_suspend_current() 登记 session → 任务入插件 worker 池
  → Lua shim：coroutine.yield()          ← actor 线程在此释放，消息继续跑
  …… worker 线程：阻塞执行原生驱动 ……
  → lua_resume_session(session, ok, result_json)   （任意线程可调）
  → 宿主经调用方 actor mailbox 恢复协程 → shim 返回 (ok, rows)
```

### Alternatives (rejection reasons)

| Option | Conclusion |
|------|------|
| callback-style `query_async(sql, params, cb)` | simplest to implement (same shape as queue.redis), but business code nests callbacks, contradicting the coroutine-based baseline of "handlers written in sequence"; not the main path |
| future/handle polling + `shield.await` | adds a whole handle-lifecycle primitive set over yield, for no benefit; rejected |
| an async vtable on `shield_database_v1` | the Lua side doesn't use the vtable (see Problem 1), so it can't save the main path; and there are no C++ consumers in-tree. Listed as a non-goal |
| per-service dedicated connection + one thread per call | connection count balloons with service count; thread cost uncontrollable; rejected |

### Why not a unified host-side wrapper

DB parameter encoding, connection pooling, and driver calls all live inside plugins (zero database code in
core is a settled boundary). The host provides only **two generic primitives**, semantically fully
isomorphic to `shield.call`:

| New host_api entries (appended to the struct tail, ABI compatible) | Semantics |
|------|------|
| `lua_suspend_current(ctx, timeout_ms, tag) -> session` | legal only inside service dispatch; wraps `suspend_for_call`; returns 0 outside a coroutine (the caller falls back to the sync path) |
| `lua_resume_session(ctx, session, ok, result_json)` | callable from **any thread**; wraps `resume_caller` (internally re-queued via the caller actor); returns non-zero when the session is already complete or nonexistent |

Plugin-side shape (mysql as the example): the namespace's methods are defined by the **plugin's own Lua
file** (`host_api.lua_add_path` already supports this), with C closures doing only submission:

```lua
-- 插件 lua/query_shim.lua（示意）
function proxy:query(sql, params)
    local session = __db_submit(self.__binding, "query", sql, params)
    if session == 0 then return __db_run_sync(self.__binding, "query", sql, params) end
    local r = table.pack(coroutine.yield())
    if not r[1] then return false, r[2] end
    return true, table.unpack(r, 2, r.n)
end
```

When `session == 0` (console eval, on_exit, non-coroutine contexts), it **falls back to today's synchronous
behavior** — the sync path continues to exist and remains always available; the discipline document keeps
applying to it.

## Semantics Contract

### Paths and Timing

| Scenario | Behavior |
|------|------|
| called inside coroutine dispatch | suspends the coroutine; SQL enters the worker pool; the actor thread keeps processing subsequent messages; results resume in completion order (no cross-call ordering guarantee) |
| non-coroutine context | synchronous execution (current semantics), blocking the calling thread |
| caller timeout | the `suspend` timeout fires → the coroutine resumes as `false, { code = "timeout", retryable = true }`; **this is not cancellation** — the native call blocking in the worker keeps running until the driver returns (next row) |
| worker completes after timeout | resume returns "session does not exist" → the worker **poisons the connection** (destroyed, not returned to the pool, consistent with the vtable transport-error discipline), logging WARNING |
| service exits with calls in flight | unfinished sessions within the drain window complete per the existing shield.call exit semantics (failure resume); worker tasks run to completion as usual and connections are reclaimed normally |
| shutdown | the plugin's shutdown first closes the worker pool's enqueue gate, drains in-flight tasks and unfinished transactions (ROLLBACK then release), then closes the connection pool |

### Transactions

The async form of `transaction` is included in this design, with one hard rule: **inside the tx body, no
yields are allowed except the tx proxy's SQL methods**. Landed shape:

- the tx proxy exposes the three SQL methods `query` / `query_one` / `execute` — one more (`query_one`)
  than the original design, to mirror the pool entry method set (any SQL read/write the pool entry can do,
  the body can do); it is likewise just SQL and does not widen the yield surface. Business code has no
  other yieldable API available; connection hold time = the sum of the individual SQL round trips, capped
  by the per-statement timeout.
- The hard rule's **runtime teeth**: calling pool-level `db:query` / `db:query_one` / `db:execute` or a
  nested `db:transaction` from the same coroutine inside the body raises immediately (a VM-level
  weak-keyed coroutine marker, see the shim). `shield.call` / `sleep` / client RPC cannot be blocked for
  all function references, so those remain **review-level** rejections (discipline document updated in
  step).
- Caller timeouts don't leak connections: the plugin automatically queues a ROLLBACK on that transaction's
  serial queue. Token-bound tasks (body statements / COMMIT / ROLLBACK) run on a **single tx lane thread**
  guaranteeing same-connection serialization; BEGIN and pool statements stay on generic workers — otherwise,
  when the pool is held to exhaustion, an `acquire` queued ahead of the lane would starve other
  transactions' COMMITs.
- The `pool.holding` gauge exposes the holding watermark (`/ops/metrics`
  `shield_plugin_db_holding`).

### Timeout Configuration

The existing layering is reused; no new required keys: a single SQL inside the worker is bounded by
`query_timeout_ms` (driver side, current state); the caller is bounded by the `suspend` timeout, defaulting
to `query_timeout_ms` + 500ms headroom, overridable per instance config via `call_timeout_ms` for special
cases. Startup validation: when the explicit config shows `caller >= driver`, a warning fires (startup is
not refused — the sync path is bound by the same config pair today).

## Observability

- Reuses `shield.pool.stats.v1` (pool capacity/usage/waiting already exist), appending two instance-level
  gauges at the tail: `pending_async` (in-flight async call count) and `holding` (transaction connection
  hold count), already in `/ops/metrics` (`shield_plugin_db_pending_async` /
  `shield_plugin_db_holding`, see Plugin Pool Stats).
- Reuses `SlowCallRing` (the shield.call slow-call ring) with tags `db:<driver>:<method>`, so slow SQL and
  slow calls share one observation convention.

## Compatibility and Migration

- The `shield_database_v1` C vtable is **kept as-is**; the host_api struct gains two entries at the tail,
  probed via `struct_size`, with no ABI version bump (compliant with abi.h stability rules).
- Lua-side method signatures, return shapes (`ok, rows` / `ok, result`), and error codes are all unchanged;
  existing scripts inside coroutine dispatch transparently change from "blocking the VM" to "suspending
  the coroutine" — invisible to correct code, with the benefit that the service's other messages no longer
  queue. Escape hatch: instance config `async: false` reverts to fully synchronous in one switch (for
  gray releases / troubleshooting).
- After landing, [DB Usage Discipline](db-discipline.md) has been downgraded per this item: rules 1/2
  (dedicated service isolation) change from "hard rules" to "recommendations"; rules 3–5 (timeouts, pool
  matching, hot reads bypassing the DB) remain enforced for both sync and async paths; rule 6 is added
  (transaction body hard rule — runtime-enforced against pool-level calls, review-enforced against
  `shield.call`/`sleep`).

## Non-Goals

- Query cancellation (all drivers are blocking APIs; timeout only guarantees the caller stops waiting, see
  the semantics contract).
- A C++-side async vtable (no in-tree consumers; a separate project if a real C++ plugin consumer appears).
- Asynchronizing the mongodb document interface (the same design applies; scheduled independently, not
  blocking the SQL trio).

## Milestones

| Stage | Deliverable | Acceptance | Status |
|------|------|------|------|
| M1 host primitives | `lua_suspend_current` / `lua_resume_session` + fake-plugin unit tests (suspend/resume/timeout/double-completion rejection/service exit) | all paths tested; the yield-window race reuses shield.call's existing guards with nothing new | done `dc2700a` |
| M2 sqlite end-to-end | the sqlite plugin's Lua shim + worker pool; actor-level real-query round-trip tests (closing the current gap: no db:query-through-actor tests exist today) | the same service can process other messages while suspended in a coroutine (timing assertion); timeout poisons the connection; `async: false` reverts to sync | done `46634f5` |
| M3 mysql/postgresql | both drivers integrated (audit of pools held across coroutines) | suspends instead of blocking the actor when the pool is exhausted (acquire moved into the worker) | done `d2b68ef` |
| M4 close-out | the tx async form + `pending_async`/`pool.holding` metrics + three document updates (lua-api.md contract, runtime-data.md, db-discipline.md downgrade) | CI Coverage all green; document conventions consistent | done |

## Open Questions (settled at landing, recorded)

1. **Shim shape**: M2 settled on **compile-time inlining** — Lua string constants in the shared C++ header
   `plugins/_shared/shield_db_async_shim.hpp`, executed via `lua.load` at proxy creation; the
   `lua_add_path` + `require` file approach was not used. All three drivers `#include` the same header,
   making physical drift impossible.
2. **`async: false` granularity**: instance-level is sufficient; no service-level key.
3. **Non-blocking driver APIs**: not adopted — the worker still calls the native driver in blocking
   fashion, keeping interface semantics unchanged; libmariadb/libpq non-blocking paths are reserved as a
   later thread-reduction optimization, as a separate project.

## Related Documents

- [DB Usage Discipline](db-discipline.md) — the usage discipline after this document's downgrade (the sync fallback path still follows it in full).
- [Data Access Architecture](runtime-data.md) — plugin autonomy boundaries, binding semantics, interface categories.
- Plugin System v1 — host_api, register_lua, ABI stability rules.
- Runtime Semantics: timers and tasks — the coroutine-based dispatch and the shield.call machine.
