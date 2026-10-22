# Timer Runtime Semantics

This document contains the runtime semantics decisions for Shield timers, sleep, and fork.

Current implementation status: `timer_once/timer/cancel_timer` are implemented; each timer is driven by a CAF driver actor (`delayed_send` + `tick_atom`), which sends `timer_fire_atom` to the owning service actor on expiry; the callback executes as a coroutine at the service actor's scheduling point via `invoke_coroutine` (it may suspend with `shield.sleep` / `shield.call` without blocking the actor), and errors route to the `on_error` hook. `shield.sleep` is coroutine-aware (yield + `_resume_after` timer resume). `shield.fork` is implemented; the task body likewise executes as a coroutine. `shield.call/call_timeout` implement coroutine-aware calls + timeout. For detailed implementation status, the Refactoring Roadmap and the Lua API Tests prevail.

**AD-07 layered clocks** are implemented: `shield.now()` returns wall-clock UTC ms (injectable `MockClock`, test-settable), `shield.monotonic()` returns real `steady_clock` ms (not settable). The no-argument forms of `os.time()`/`os.date()` are redirected to the business clock; `os.time(table)`/`os.date(fmt,t)`/`os.clock()` keep native behavior. `LuaServiceManager::attach_clock()` injects the clock; `Clock`/`SystemClock`/`MockClock` are defined in `include/shield/lua/clock.hpp`. `test_lua_api_clock` covers 12 cases.

## timer API

```lua
local id = shield.timer_once(1000, function()
end)

local id = shield.timer(1000, function()
end)

local ok = shield.cancel_timer(id)
```

Rules:

- The timer callback executes as a coroutine in the current service's Lua VM (wrapped by `invoke_coroutine`); errors route to the `on_error` hook.
- Inside the callback, `shield.sleep`/`shield.call` suspend the callback coroutine and the actor immediately returns to the mailbox to process other messages; the existing resume mechanism continues after the callback finishes.
- Timers belong to the current service.
- On service exit, owned timers are cancelled automatically.
- `TimerId` is an opaque userdata; CAF is not exposed.

## timer Time Semantics

```txt
timer_once(delay_ms)
timer(interval_ms)
```

Rules:

- `delay_ms >= 0`.
- `interval_ms > 0`.
- **Layered time (AD-07)**: timer **firing** is based on the monotonic clock (InfraClock, not settable), consistent with CAF `delayed_send`. `shield.now()` / `os.time()` (no args) / `os.date()` (no args) read by Lua business go through the injectable wall-clock `Clock` (LuaClock, settable in tests). `os.clock()` is not wired in (keeps real CPU time). See [Architecture Decision AD-07](architecture-decisions.md).
- Periodic timers use fixed-cadence semantics: the next tick advances as "previous fire time + interval_ms", independent of callback execution duration.

fixed-cadence:

```txt
the driver actor re-arms the next tick's delayed_send immediately after dispatching this tick's fire;
next tick time = this tick's fire time + interval_ms
```

- Slower callback execution neither shifts the cadence base nor accumulates missed ticks for catch-up — ticks already past are not re-fired.
- The drift cap of the actual fire time relative to the theoretical tick is the fire message's dispatch delay (the driver-side recorded `next_fire_ms` advances each tick to "current time + interval_ms"; the comment's words: drift equals the fire-dispatch lag).
- Fixed-rate is not used, avoiding catch-up pileup from slow callbacks — the motivation is unchanged; only the cadence base is fixed and does not shift with callback completion time.

## timer Error Semantics

When a timer callback raises an error:

- The error routes via `invoke_error_hook` to the service's `on_error(err, context)` hook with `context.type = "timer"`, and a `timer error: ...` ERROR log is recorded.
- `timer_once` ends after recording the error (single fire; the entry is removed after firing).
- `timer` **keeps firing on cadence after recording an error and does not stop automatically** — the implementation has no "stop on error" logic; what can stop it are an explicit `cancel_timer`, service exit (auto-cancels owned timers), or the panic exit below.
- Reaching the consecutive error threshold (`kDefaultMaxErrorsBeforePanic`, currently 10; one successful dispatch resets the counter) triggers `on_panic` and `exit("panic")`.
- Errors feed `shield_ops` statistics (Phase 2, not yet implemented).

Because periodic timers keep running on error, business code only needs its own `pcall` when it wants certain errors not to spam `on_error`/counting.

## shield.sleep

```lua
shield.sleep(1000)
```

Semantics:

- In a coroutine context (message handler, timer/fork callback, on_init, on_shutdown) only the current coroutine is suspended.
- Only in coroutine-less contexts such as module-level code does it degrade to the thread-blocking `_block_sleep`.
- The coroutine path does not block the runtime thread.
- Implemented on top of timers.
- On service exit, sleeping coroutines are cancelled.
- Business code needing non-blocking waits should call it inside service handlers.

`sleep(0)` yields the current coroutine to the tail of the service ready queue.

## shield.fork

`shield.fork` posts a background task owned by the current service to the owning service actor (executed at the actor's scheduling point via `fork_task_atom`); it does not create a new service.

```lua
local task_id = shield.fork(function()
  shield.send("player", "tick")
end)
```

Rules:

- A fork task shares the Lua VM and Lua state with the current service.
- A fork task has no service id, no mailbox, and no name.
- The fork task executes as a coroutine (wrapped by `invoke_coroutine`); `shield.sleep` / `shield.call` suspend the task coroutine inside it without blocking the actor.
- Forks scheduled inside `on_init` execute serially only after init completes: the actor stashes `fork_task_atom` during init and unstashes them uniformly at `init_ready` — a fork never enters the same Lua VM concurrently with the spawning thread.
- Fork unhandled errors are logged and counted in ops metrics; they do not kill the service.
- On service exit, owned fork tasks are cancelled automatically.

## Task ID

`shield.fork` currently returns a numeric task id.

```lua
local task_id = shield.fork(fn)
```

The current contract provides no task cancel, status, or join, avoiding extra wait graphs and deadlock semantics. When a result is needed, use a service `call` or a business channel.
