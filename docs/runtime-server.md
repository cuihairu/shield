# Server State Runtime Semantics

This document freezes the implementation contract of the `shield_server` optional module P0: the server state machine, runtime info, state change notification, and shutdown handover. Decision rationale in [open-decisions](open-decisions.md) OD-016/OD-017; the module's cross-cutting boundaries in the Official Optional Module Contracts.

`shield_server` is an official optional module and is not part of `shield_core`. Build switch `SHIELD_ENABLE_SERVER` (default OFF). With the module disabled, bootstrap / core do not depend on `server_manager`, and the runtime starts, runs, and shuts down gracefully as usual.

## Core Decisions

- ServerManager is a process-level C++ singleton (in the PlayerManager mold: `global()/set_global()`, bootstrap holds the `unique_ptr`). It is not a service: no mailbox, no CAF actor, no spawned service, no Starter needed.
- Server state is separate from player state. ServerManager keeps only server-level state and read-only runtime info.
- The state truth lives on the C++ side; Lua queries it read-only or migrates it explicitly through the `shield.server.*` facade.
- State change notifications are delivered to watcher services over the existing service messaging system channel; no global event bus is introduced.
- `shutdown(ms)` does exactly three things: "migrate + notify + schedule a stop request"; the real drain budget and graceful shutdown order remain on bootstrap's `shutdown.timeout.*` path.
- Maintenance mode at P0 only provides the `state()` truth query; the login admission gate is implemented by the business / `shield_player` layer itself — core and the gateway are unaware of maintenance state.

## State Machine

| State | Meaning | Behavior |
|------|------|------|
| `starting` | starting up | initial state during bootstrap initialization |
| `running` | normal operation | entered automatically after init complete |
| `maintenance` | maintenance mode | business semantics; the runtime and service paths are unaffected |
| `shutdown` | shutting down | terminal state; no migrations are accepted after entering |

Legal transitions (`transition_allowed`, a pure function, table-driven):

```text
starting -> running       (bootstrap init complete auto-triggers, mark_ready)
starting -> shutdown      (shutdown may be requested during startup)
running  -> maintenance
running  -> shutdown
maintenance -> running
maintenance -> shutdown
```

Illegal transitions (return an error; no crash, no state change):

- `starting -> maintenance`: startup must complete before entering maintenance.
- Anything `-> starting`: `starting` can only be the initial state.
- Anything after `-> shutdown`: `shutdown` is terminal.

A same-value `set_state` (e.g. `running -> running`) succeeds idempotently and does not trigger notification.

```text
┌────────────┐  init complete   ┌────────────┐
│  starting  │ ───────────────► │  running   │◄──────────┐
└─────┬──────┘                  └─────┬──────┘           │
      │ shutdown                      │ maintenance      │ resume
      ▼                               ▼                  │
┌────────────┐             ┌──────────────┐             │
│            │ ◄────────── │ maintenance  │ ────────────┘
│  shutdown  │             └──────┬───────┘
│  (terminal)│ ◄─────────────────┘ shutdown
└────────────┘
```

## Configuration

The owner is the `server_manager` config section (Runtime Config Semantics Optional Module Schema). All fields are optional; defaults apply when unconfigured:

```yaml
server_manager:
  name: "server_manager"       # node name for observability display, not a service name
  info:
    name: ""                   # business display name
    version: ""                # business version; the preferred source for shield.server:version()
    region: ""                 # deployment region label
```

Validation rules: an empty `name` string is invalid; `info.*` over 64 characters is invalid. Validation failure follows the optional module default fail-fast policy (OD-005), with the error located at the `server_manager` config path.

There are no `enabled` / `state` / `on_state_change` fields: enablement is decided solely by the build switch; the initial state is always `starting`; the notification switch is expressed by watch registration behavior.

## Lua API

`shield.server` is the facade provided by `shield_server` (in the `shield.player` mold). With the module not compiled, every entry returns `nil + {code="module_unavailable"}`; with the module compiled but no runtime instance installed, it likewise returns `module_unavailable`.

```lua
local server = shield.server

-- read-only queries
server:state()        -- "starting" | "running" | "maintenance" | "shutdown"
server:uptime()       -- steady seconds since the running point (float)
server:version()      -- info.version, falling back to app.version, "" if both empty
server:node_id()      -- cluster node_id; "" when standalone
server:started_at()   -- wall-clock millisecond timestamp of the running point
server:config()       -- {name=..., info={name=..., version=..., region=...}}

-- state control (returns true or nil + Error)
server:set_state("maintenance")   -- illegal transition → {code="invalid_state_transition"}
server:shutdown(30000)            -- request process stop after 30 seconds

-- state watching
local id = server:watch(fn)       -- register a watcher, returns a watch id
server:unwatch(id)                -- unregister
```

### set_state

- The argument must be one of the strings `starting/running/maintenance/shutdown`; otherwise `nil + {code="invalid_state"}`.
- An illegal transition returns `nil + {code="invalid_state_transition", message="invalid state transition: a -> b"}`.
- A legal transition returns `true` and triggers notification; a same-value idempotent call returns `true` without notifying.

### shutdown

`server:shutdown(ms)`:

1. Validates the argument: `ms` must be an integer ≥ 0, otherwise `nil + {code="invalid_argument"}`.
2. Migrates the current state to `shutdown` (legal from `running/maintenance/starting`; if already in `shutdown`, returns `nil + {code="shutdown_already_scheduled"}`).
3. Immediately delivers one `"shutdown"` notification to all watchers.
4. After `ms` milliseconds, requests process stop — the same stop path as SIGINT/SIGTERM (`shield::request_stop`).

The drain budget, service exit order, and timeout force-kill are still decided by bootstrap's `shutdown.timeout.*`; `shutdown(ms)` neither takes over nor shortens those budgets. Repeated calls return `shutdown_already_scheduled` and never restart the timer.

### watch / unwatch

```lua
-- register a watcher inside the service module
function M.on_init(args)
    shield.server:watch(function(ctx, new_state)
        if new_state == "shutdown" then
            M:save_all_data()
        end
    end)
end
```

- The callback signature is the equivalent of `M.on_server_state_change`: watch accepts a function directly; at dispatch the first parameter is ctx (same rule as handler dispatch) and the second is the new state name string.
- Repeated `watch` by the same service is idempotent: the same id is returned, no duplicate registration.
- Callbacks execute serially on the watcher's own service actor thread, never concurrently with that service's other messages.
- Delivery goes through the system message channel (`send_system`), the same path as the gateway bridge; a missing watcher handler is a silent no-op, not an error.
- When a watcher service exits, the runtime unregisters it automatically (`service_not_found` → registration removed); delivery failures while the runtime is shutting down do not unregister — the shutdown path handles those.
- `unwatch(id)`: a nonexistent or already-unregistered id succeeds silently (idempotent).

## Runtime Info

| Field | Source |
|------|------|
| `state` | ServerManager state machine |
| `uptime` | steady clock seconds since the running state's start |
| `started_at` | wall-clock ms of the running point (`0` means running not yet entered) |
| `version` | `server_manager.info.version` → fallback `app.version` → fallback `""` |
| `node_id` | the cluster module's node_id; `""` when standalone |
| `name` / `info` | the `server_manager` config snapshot |

`uptime` / `started_at` count from entering `running` (the `mark_ready` moment), not from process start; during `starting`, uptime is 0.

## ops / console Exposure

P0 has no standalone `/ops/server` endpoint. Server state is merged as a read-only snapshot into two existing observation surfaces:

- The `GET /ops/status` response gains a `server` block: `{state, uptime_seconds, version, node_id, started_at_ms, name, info, watchers}`.
- The diagnostics console gains the `root.server` command, printing the same JSON; the `root.status` overview likewise includes a server summary.

With the module uncompiled / disabled, `root.server` prints a clear notice and `/ops/status` has no `server` block. ops is read-only; there is no HTTP/console entry for changing state (state control goes only through the Lua API).

## Error Codes

| Error code | Scenario | retryable |
|--------|------|-----------|
| `module_unavailable` | `SHIELD_ENABLE_SERVER` not compiled, or the module is uninitialized | no |
| `invalid_state` | the `set_state` argument is not a legal state name | no |
| `invalid_state_transition` | a transition the state machine does not allow | no |
| `shutdown_already_scheduled` | `shutdown` called repeatedly | no |
| `invalid_argument` | the `shutdown` argument is not a non-negative integer | no |

Errors belong to the `shield_server` module domain (the error ownership table in the Architecture); runtime stop failures during the shutdown flow still belong to the bootstrap/core error domains.

## Explicit Non-Goals

- Cross-node state broadcast (each node independently holds its own ServerManager).
- A built-in maintenance admission gate in core/gateway (implemented at the business layer).
- A standalone `/ops/server` HTTP endpoint and ops write operations.
- A ServerStarter (ServerManager takes no part in Starter orchestration).
- A global event bus (notifications go only to explicitly registered watchers).

External shutdown (SIGINT/SIGTERM, console stop, etc.) is handled by bootstrap uniformly migrating ServerManager to the `shutdown` state at the `shutdown()` entry (landed, P1): watcher notifications are still delivered, but external shutdown provides no Lua observation window — whether callbacks execute before teardown depends on scheduling and is not part of the contract.
