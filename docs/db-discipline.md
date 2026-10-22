# DB Usage Discipline

> Source: the [HIGH] risk conclusion of `docs/architecture-review.md` §4 — the synchronous call path of
> `shield.database.v1` blocks (the database.h vtable has no callback/future parameters). The async entry
> points have landed ([DB Async Entry Points](db-async-design.md)): inside coroutine dispatch,
> `query` / `query_one` / `execute` / `transaction` suspend the caller's coroutine and the SQL runs on the
> plugin's workers, so the service keeps processing other messages. Therefore **rules 1 and 2 are downgraded
> from hard rules to recommendations** (blocking is isolated by the async mechanism itself, no longer gated
> by review); **rules 3–6 remain hard rules** — the synchronous fallback path (non-coroutine contexts,
> `async: false`) still stalls the VM as before, and the connection pool, timeout, and transaction hold
> disciplines do not lapse because of async.

## Why this discipline exists

Each Shield service is a single-threaded actor with an exclusive Lua VM. Whichever service makes a
synchronous DB call, **all of that service's messages queue for exactly as long as the call takes**:

```text
db:query(...) blocks 3s
  → the service's VM stalls 3s
  → every call / client RPC / timer callback arriving at that service during that window is delayed 3s
```

- A player service stalled 3s: that player perceives disconnection, unresponsive controls — tolerable.
- A shared service (world / ranking / chat / matchmaking) stalled 3s: **the whole server stutters**.
- A timer callback stalled 3s: all of that service's other timers are pushed back.

The skynet ecosystem likewise has no built-in DB, but its conventions (suspending agents + a separate
driver service) isolate blocking naturally; Shield now achieves the same effect with the same shape —
coroutine suspension + plugin workers — done at the mechanism level by the async entry points. The
**synchronous blocking** cost described above is still real on the fallback path (non-coroutine contexts,
`async: false`); in those cases this document's discipline is the entire line of defense.

## Recommendations (mechanism-guaranteed after the async entry points landed; review no longer enforces)

1. **Put DB calls in dedicated DB services by default** (low-concurrency services with no client hot
   path, like `db.player` / `db.audit` / `db.guild`). Shared services and player hot-path services can
   now call database bindings directly inside coroutine dispatch — the call suspends rather than stalling
   the VM, and "one slow query stalls the whole server" no longer holds. But isolation remains the default
   shape: suspended call chains are still subject to timeouts and pool waits; review won't block them,
   the business carries the risk.
2. **Business code reaches the DB service via `shield.call` / `shield.call_timeout`**. The async entry
   points removed the hard constraint that "direct calls stall the VM", but the persistence discipline of
   in-memory state first + dirty-flag periodic snapshots on the player online hot path is unchanged (see
   the "async snapshot save" of Persistence Patterns).

## Still Hard Rules

3. **Timeouts must be configured explicitly and satisfy `call_timeout < query_timeout`**:
   - Plugin instance: `query_timeout_ms` (default 5000), `acquire_timeout_ms`
     (default 10000), `connect_timeout_ms` (default 5000).
   - Caller: the T in `shield.call_timeout(T, ...)` must be smaller than the query timeout, leaving a
     time window for error handling; otherwise during a slow query the caller's coroutine hangs and
     pending_calls pile up.
   - The same applies to the coroutine suspension budget of the async entry points: when the instance's
     configured `call_timeout_ms` (default `query_timeout_ms + 500`) is explicitly ≥ `query_timeout_ms`,
     a startup warning fires — the caller expires first and late completions have no claimant
     (discarded + WARN log).
4. **Match instance count to the pool**: start a DB service with `instances: 1` plus the plugin's
   `pool_size` (default 4). Do not stack concurrency with multiple DB service instances — the connection
   pool is the concurrency gate; also do not spawn a private DB service per player.
5. **Hot reads bypass the DB**: high-frequency reads like leaderboards and online lists go through
   memory / global / Redis cache plugins; the DB is only the cold source and backfill.
6. **Inside a `transaction` body, use only the `tx:` handle for SQL**. An async transaction holds one
   connection exclusively while suspended, with an occupancy ceiling of the single statement's timeout
   budget; this "connection holding must have a hard cap" is enforced by the runtime: calling pool-level
   `db:query` / `db:query_one` / `db:execute` or a nested `db:transaction` inside the body raises
   immediately (review no longer needs to block it). Calling `shield.call` / `shield.sleep` inside the
   body remains a review-level ban — that would introduce unbounded suspension points while holding the
   connection.

## Correct Shapes

A dedicated DB service (`scripts/db_player.lua`) — the only place `db:query` may appear:

```lua
-- db_player.lua - dedicated DB service: the sole funnel for DB access; business enters via shield.call
local M = {}

local db

function M.on_init(args)
    -- the binding logical name comes from config plugins.bindings; no instance id is passed
    db = shield.database.mysql("database.default")
end

-- read: single/multi-row SELECT
function M.load_profile(uid)
    local ok, rows = db:query(
        "SELECT nickname, level, gold FROM players WHERE uid = ?", { uid })
    if not ok then
        return false, "db_read_failed"
    end
    return true, rows[1]  -- nil on empty result; the caller handles it
end

-- write: debit-style operations go through a transaction + idempotency key (see runtime-persistence.md synchronous strong save)
function M.debit(uid, amount, op_id)
    local ok, err = db:transaction(function(tx)
        tx:execute(
            "INSERT INTO op_log(op_id, uid, amount) VALUES(?, ?, ?)",
            { op_id, uid, amount })  -- the idempotency key lands first; a unique-key conflict = duplicate request
        tx:execute(
            "UPDATE wallet SET gold = gold - ? WHERE uid = ? AND gold >= ?",
            { amount, uid, amount })
    end)
    if not ok then
        return false, err or "db_write_failed"
    end
    return true
end

return M
```

The business side (a player service or any shared service) — only `shield.call`, no binding:

```lua
-- player.lua online hot path: memory first, DB delegated to the dedicated service
function M.buy(ctx, client, request)
    local amount = tonumber(request.amount) or 0
    if amount <= 0 then
        return { code = "bad_amount" }
    end

    -- write path (strongly consistent assets): call_timeout < the plugin's query_timeout (5s)
    local ok, result, reason = shield.call_timeout(4000,
        "db_player", "debit", client:player_id(), amount, request.op_id)
    if not ok or result == false then
        return { code = reason or "debit_failed" }
    end

    -- memory takes effect immediately; async snapshots back up persistence
    M.wallet[client:player_id()] = (M.wallet[client:player_id()] or 0) - amount
    return { code = "ok", gold = M.wallet[client:player_id()] }
end

function M.load_profile(uid)
    -- cold read: only low-frequency paths like login/costume change may pass through the DB service
    return shield.call("db_player", "load_profile", uid)
end
```

Configuration (plugin instance + binding + explicit timeouts):

```yaml
plugins:
  instances:
    - id: "mysql-main"
      package: "database.mysql"
      config:
        host: "127.0.0.1"
        port: 3306                  # classic MySQL protocol
        database: "game"
        user: "game"
        password: "..."
        query_timeout_ms: 5000      # per-statement SQL cap
        acquire_timeout_ms: 10000   # pool wait cap
        pool_size: 4
  bindings:
    - name: "database.default"      # the business side passes this logical name
      instance: "mysql-main"

actors:
  - name: "db_player"               # dedicated DB service: low concurrency, no client listener
    script: "../scripts/db_player.lua"
    instances: 1
```

## Counter-Examples (review sends these straight back)

```lua
-- counter-example 1: a shared/hot-path service queries the DB directly — no longer a whole-server
-- stall incident after async landed; downgraded to a style issue: slow queries still suspend the
-- call chain and still occupy the pool. The default shape remains isolation.
-- (world.lua)
function M.on_enter_room(uid)
    local ok, rows = shield.database.mysql("database.default"):query(
        "SELECT ... FROM players WHERE uid = ?", { uid })  -- △ review reminder, not blocked
    ...
end

-- counter-example 2: a bare call with no timeout — the caller's coroutine suspends without limit
-- during slow queries
local ok, profile = shield.call("db_player", "load_profile", uid)  -- counter-example: hot path
-- strongly consistent write paths should also cover query_timeout with call_timeout

-- counter-example 3: one DB service per player — connections = player count, punching through the
-- connection pool
-- config: instances: N × player_service + every service grabs the binding    -- counter-example

-- counter-example 4: passing conn/tx objects across services — they are bound to the calling
-- service's synchronous execution context; passing them across services infects the other side
-- with blocking                                              -- counter-example
```

## Observability and Damage Control

- `/ops/metrics`'s `shield_service_pending_calls{service="db_player"}` climbing steadily = the DB side
  is slowing; look at the DB before growing the pool; read `shield_service_requests_total` and errors
  alongside. Conventions in Ops Runtime Semantics.
- Direct callers look at the plugin-side gauges: `shield_plugin_db_pending_async{plugin=...,instance=...}`
  climbing steadily = in-flight async calls piling up; `shield_plugin_db_holding` high for a long time =
  a transaction holding a connection for a long time (compare against `pool_size`).
- A slow DB service only slows the businesses that explicitly `shield.call` it; it cannot take down shared
  services — that is the blast-radius control the isolation discipline buys; in direct-call cases coroutine
  suspension protects the service itself.
- Damage-control order: alert (pending_calls threshold) → business degradation (skip non-critical writes,
  fall back to memory) → handle the DB side.

## When It Can Be Relaxed

- Ops/management cold paths (GM tools, offline batch jobs) may relax the timeout requirements beyond the
  recommendations of rules 1 and 2, but rule 3's explicit timeout configuration itself remains mandatory.
- The async entry points (coroutine-resume style, [DB Async Entry Points](db-async-design.md)) have
  landed, and rules 1 and 2 are downgraded to recommendations as planned. Note the downgrade's scope:
  **only calls inside coroutine dispatch with the instance not configured `async: false` suspend**;
  non-coroutine contexts (like `on_init`) take the synchronous fallback, still blocking the VM — the old
  rules apply in full there. Rules 3–6 are not downgraded by the landing.
