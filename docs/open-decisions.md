# Decision Log

This document centrally records design details that have already converged and questions still awaiting a ruling. When closing a decision, the corresponding authority document must be updated in step.

Authority document precedence:

1. Final Architecture Overview
2. Lua API Contract
3. Runtime Config Semantics
4. [Runtime Semantics Decisions](runtime-semantics.md)
5. Official Optional Module Contracts
6. Refactoring Roadmap

## Closed Decisions

### OD-001 CLI Contract

Status: closed. Authority document: [Bootstrap Runtime Semantics](runtime-bootstrap.md).

Decisions:

- `--config <path>` may be omitted; the default is `config/app.yaml`.
- `--config` may be passed repeatedly; multiple files merge in the order given, with merge rules governed by
  Runtime Config Semantics.
- `--node-id` belongs to the `shield_cluster` optional module. Passing it without cluster enabled must return
  a CLI error.
- Old subcommands exist only as legacy CLI and do not enter the new `shield::run` public contract.
- The `shield` executable and example executables should all call `shield::run(argc, argv)` directly, so they
  share one parameter set.

### OD-002 Process Lifecycle

Status: closed. Authority document: [Bootstrap Runtime Semantics](runtime-bootstrap.md).

Decisions:

- `shield::run` handles generic signal handling, the runtime loop, help/version output, and exit codes.
- Windows uses `SetConsoleCtrlHandler`; POSIX uses SIGINT/SIGTERM.
- `shield::run` initializes the runtime then blocks waiting for a stop signal; the lower-level bootstrap API
  only handles initialize/shutdown.
- A user-defined main may bypass `shield::run` and call the lower-level bootstrap API directly.
- Exit codes follow [Bootstrap Runtime Semantics](runtime-bootstrap.md#进程退出码).

### OD-003 Physical Directory Layout

Status: closed. Authority documents: Directory Structure Design and the
Refactoring Roadmap.

Decisions:

- Short term, keep the current physical directories `src/core`, `src/net`, `src/lua`, etc.; converge
  dependencies first with CMake targets and include boundaries.
- Phase 1 does not simultaneously do large-scale file moves and semantic refactoring.
- Optional modules' target physical directories remain shapes like `src/optional/shield_cluster`, created or
  migrated only when the corresponding module is implemented.
- The public include path stays `include/shield/...`, with no breaking renames following target names.

### OD-004 Phase 1 Network Scope

Status: closed. Authority document: [Network Runtime Semantics](runtime-network.md).

Decisions:

- Phase 1 freezes only TCP sessions and basic transport framing.
- UDP, KCP, and WebSocket remain target semantics and later extensions, not Phase 1 acceptance blockers.
- A business HTTP gateway does not enter core; HTTP management endpoints exist only when `shield_ops` is
  explicitly enabled.

### OD-005 Optional Module Failure Policy

Status: closed. Authority documents: Official Optional Module Contracts and
Runtime Config Semantics.

Decisions:

- When configuration contains an optional module section but the module is not built or not enabled, startup
  fails.
- An explicitly enabled optional module failing to initialize fails fast by default.
- `shield_cluster`'s first version allows degrading to single node when remote connections fail, but must
  continuously expose the unhealthy state; invalid configuration, node identity conflicts, and local listener
  failures still fail fast.
- After `shield_ops` is enabled, a management port binding failure must fail fast.
- `shield_global`, `shield_player`, and `shield_server` initialization failures fail fast by default.

### OD-006 Data API Phase Split

Status: closed. Authority documents: [Data Access Runtime Semantics](runtime-data.md) and the
Lua API Contract.

Decisions:

- Data access does not enter `shield_core` and is no longer exposed through a separate `shield_data` module.
- The current Phase 1 contract is Plugin System v1: manifest scan, explicit instances, explicit bindings,
  C ABI interfaces, and plugin self-registered Lua namespaces.
- SQL, document store, cache, queue, and leaderboard capabilities are each provided by provider plugins.
- Business Lua uses plugin namespaces + binding logical names, e.g.
  `shield.database.mysql("database.default")`.
- `shield.db.*` / `shield.redis.*` are legacy APIs, deleted with no public compatibility shim.
- Prepared statements, pipelines, eval, sentinel/cluster, and similar capabilities evolve independently in
  specific plugins or later plugin interfaces; they never expand the core API in reverse.

### OD-007 Legacy Compatibility Window

Status: closed. Authority documents: the Refactoring Roadmap and the
Lua API Tests.

Decisions:

- No public compatibility shim is provided.
- Legacy Lua API deletion is gated on the negative tests of `lua-api-tests.md`.
- Legacy C++ target deletion is gated on examples/tests no longer referencing them.
- Old CLI subcommands may exist short-term as legacy CLI code, but do not enter the new `shield::run` public
  contract.

### OD-008 Player Setup API Form

Status: closed. Authority documents: Player Lifecycle Runtime Semantics and the
Official Optional Module Contracts.

Decisions:

- P0 freezes only `shield.player.setup(M, opts)` as the sole recommended entry point for business Player
  Services.
- `shield.player.Base` stays as an advanced style for Phase 2+; not implemented in P0, with the P2 boundary
  in OD-014.
- setup's opts fields drop the `on_` prefix (`auth`/`login`/`client_message`/`disconnect`/`reconnect`/`logout`/`save`),
  keeping a separate namespace from module-level `on_init/on_exit`.
- Missing required hooks (`auth`/`login`/`client_message`/`disconnect`/`logout`) returns
  `nil, Error{code="setup_invalid"}` immediately; the service never enters the running state.
- Optional hooks not provided in setup execute the framework's documented default implementations; implicit
  "not provided means noop" behavior is not allowed.
- When business overrides an optional hook, the default does not auto-run; keeping the default behavior
  requires explicitly calling the `shield.player.defaults.*` helpers.

### OD-009 Player Persistence Adapter Boundary

Status: closed. Authority documents: Player Lifecycle Runtime Semantics and the
Official Optional Module Contracts.

Decisions:

- The persistence adapter is a lightweight contract owned by `shield_player`; it does not belong to
  `shield_data`.
- The adapter's底层 must access data capabilities through plugin namespaces + binding logical names; it does
  not re-define SQL/Redis/document store semantics.
- The adapter owns no connection pools; pools belong to the corresponding plugin instance.
- The adapter introduces no ORM, mappers, or schema toolchain; it accepts only whitelist fields encodable by
  LuaPack.
- Adapter failures reuse the corresponding data plugin error codes; the player domain only adds errors like
  `persistence_save_failed` that clearly belong to this module.
- With no persistence configured, `on_save`'s default implementation is a no-op.

### OD-010 Player Cross-Service Reference

Status: closed. Authority documents: Player Lifecycle Runtime Semantics and the
Official Optional Module Contracts.

Decisions:

- The cross-service player reference is named `PlayerRef`, not `PlayerHandle`, avoiding confusion with
  `ServiceHandle`.
- `PlayerRef` is a player-module-internal reference, **not** a replacement for `ServiceHandle`.
- The `PlayerRef` structure is `{ uid, node_id, service_id, epoch }`; `epoch` comes from shield_cluster's
  `node_epoch` and is 0 on a single node.
- Cross-service payloads may **only** carry `PlayerRef` (or the client RPC's read-only
  `ClientContext`/`ClientRef` tokens); bare connection handles or full `PlayerSession`s are forbidden.
- `shield.player.resolve(ref)` at P0 freezes only local resolve; remote resolve is Phase 2+, defined jointly
  by `shield_cluster` + `shield_player`.
- An invalid ref returns `nil, Error{code="player_not_found"}`; it does not throw.

### OD-011 Player Extended States

Status: closed. Authority document: Player Lifecycle Runtime Semantics.

Decisions:

- `anonymous` and `spectator` are both opt-in states; the default state machine remains
  `connecting -> authenticating -> online`.
- The `auth` hook stays required; business declares anonymous or spectator connections via auth's return
  value.
- When not explicitly enabled, `anonymous_disabled` or `spectator_disabled` is returned.
- Upgrading anonymous to a formal account must re-authenticate and generates a new `PlayerRef`.

### OD-012 Player Scale Model

Status: closed. Authority document: Player Lifecycle Runtime Semantics.

Decisions:

- The default model is `service_per_player`.
- `player_pool` is an optional operating strategy for large-scale scenarios; it does not change the
  `shield.player.*` public API.
- Under `player_pool`, `PlayerRef.service_id` points at a pool shard; per-player routing is done by
  `shield_player` within the shard.
- Before implementation, benchmarks for memory, latency, GC, mailbox backlog, and reconnect-window caching
  must be added.

### OD-013 Player Multi-Device Policy

Status: closed. Authority document: Player Lifecycle Runtime Semantics.

Decisions:

- The default policy is `single`.
- `kick_old` makes the old session exit with the `logout` reason `"replaced"`.
- `multi` uses a `(uid, device_id)` index; `shield.player.get(uid)` returns the primary session; device
  enumeration goes through `PlayerManager.get_devices(uid)`.
- Without a stable `device_id`, only temporary sessions are allowed, which do not participate in same-device
  reconnect.

### OD-014 Player Base API Boundary

Status: closed. Authority document: Player Lifecycle Runtime Semantics.

Decisions:

- `shield.player.Base` is P2 syntactic sugar and must be based on `shield.player.setup`.
- Base introduces no second lifecycle, supports no multiple inheritance, and does not restore the `on_*`
  prefix for setup fields.
- P0/P1 tests use setup as the norm; Base tests only verify there is no extra semantics.

### OD-015 Runtime Primitives Scope

Status: closed. Authority documents: [Base Components and Runtime Adaptation Boundaries](runtime-primitives.md),
the Refactoring Roadmap, and [Network Runtime Semantics](runtime-network.md).

Decisions:

- Runtime primitives like `shield.buffer` / `shield.crypto` / `shield.socket` / `shield.stream` /
  `shield.tls` remain a deferred direction only and do not enter the current main path.
- Cosocket-style APIs are not put into the current default mental model for business users.
- The current recommended mental model keeps three things: client ingress goes through the gateway's single
  target + read-only `ClientContext`/`ClientRef`; inter-service goes through `shield.send/call`; external
  HTTP goes through `shield.http`.
- If primitives advance in the future, they should be an advanced or deferred-phase capability, not the
  current default recommended path.
- JWT/Auth is not a base-component-layer central capability and is not released as an official Shield plugin;
  business layers should implement it on the base primitives or external services.

Follow-up execution (2026-09-28, AD-08): the first `shield.crypto` slice has landed (codec / SHA-256 /
HMAC-SHA256 / `random_bytes` / `constant_time_compare`), making the "business layer on base primitives"
phrasing above workable;
the reference implementation is the Lua-side `scripts/lib/jwt.lua`, and `plugins/auth_jwt` is marked
deprecated (kept for one transition period). The remaining primitives (`shield.buffer` / `socket` /
`stream` / `tls`) are still deferred directions.

### OD-016 Server State Machine and API Surface

Status: closed. Authority documents: [Server State Runtime Semantics](runtime-server.md) and the
Official Optional Module Contracts.

Decisions:

- ServerManager is a process-level C++ singleton (in the PlayerManager mold: `global()/set_global()`), not a
  service: no mailbox, no CAF actor, no spawned services, no Starter orchestration.
- The state machine is `starting → running ⇄ maintenance`, `running/maintenance/starting → shutdown`;
  `shutdown` is terminal; `→ starting` is always illegal; `starting → maintenance` is illegal; same-value
  set succeeds idempotently without notifying; illegal transitions return errors without crashing.
- `starting → running` is auto-triggered by bootstrap init complete (`mark_ready`), not exposed as an API.
- The config section is only `server_manager.{name, info.{name,version,region}}`, all optional; the draft's
  `enabled` (enablement is decided only by the build switch), `state` (the initial state is always
  starting), and `on_state_change` (the notification switch is expressed by watch registration) fields are
  deleted.
- State watchers register as `shield.server:watch(fn)`; delivery must go through the system message channel
  (`send_system`) — ordinary `send()` silently rejects `on_`-prefixed methods, and state notification method
  names are in the reserved prefix domain. The callback signature carries the dispatch ctx
  (`fn(ctx, new_state)`), consistent with handler dispatch rules.
- The "C++ registry holds Lua callback objects" option is rejected: it would make the pure-C++
  ServerManager link the script binding layer, destroying unit-testability. The C++ registry stores only
  `{watch_id, service_id}`; watcher callbacks are held by each VM's orchestration layer, which installs
  forwarding handlers; when a watcher service exits it is unregistered by liveness determination.

### OD-017 Server Shutdown Handover and Maintenance Boundary

Status: closed. Authority document: [Server State Runtime Semantics](runtime-server.md).

Decisions:

- `server:shutdown(ms)` does exactly three things: migrate to `shutdown`, notify watchers, and after `ms`
  milliseconds issue the process stop request on the same path as SIGINT/SIGTERM. It does not take over the
  drain budget — timeouts and exit order remain on bootstrap `shutdown.timeout.*`.
- Repeated `shutdown` returns `shutdown_already_scheduled` without restarting the timer; the timer thread
  uses `std::jthread` + stop_token, and both bootstrap cleanup paths stop it explicitly.
- Maintenance mode at P0 provides only the `state()` truth query; the login admission gate is implemented by
  the business / `shield_player` layer; core and the gateway are unaware of maintenance state.
- State notification callbacks enter the messaging layer via the bootstrap-injected `notify_fn` callback;
  ServerManager does not depend on `shield_lua` directly, avoiding a circular dependency.
- ops exposure merges into `/ops/status`'s `server` block and the console `root.server` command; no
  standalone `/ops/server` endpoint and no ops write entry.

## Open Decisions

There are currently no base semantic questions still awaiting a design ruling. If new open questions appear
later, record them in this section first, then update the corresponding authority documents in step.
