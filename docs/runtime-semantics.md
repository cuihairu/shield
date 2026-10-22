# Runtime Semantics Decisions

This document records Shield's post-refactoring runtime semantics decisions; it is the target contract for coding implementations, and the landed status of individual items is annotated per section.

The goal is that future implementers need not re-discuss base semantics and can extend tests and iterate against this document.

## Design Principles

- `shield_core` carries only the Actor/Service core semantics.
- CAF is an internal mechanism; it does not appear in the Lua API or the public C++ API.
- Lua is the default business language; C++ handles the runtime and a few performance-sensitive extensions.
- Message semantics for local, IPC, and cluster must be as consistent as possible.
- The minimal deployment path prioritizes single-node stability; `shield_cluster` is an official optional module that stays out of `shield_core`, but address, timeout, and error semantics need to be frozen early.

## Semantics Document Index

Runtime semantics have been split into separate documents by topic. When reading and implementing now, apply the following precedence.

Current convergence order:

- The Phase 1 minimal loop covers single-node Lua services, the registry, the basic `send/call` return shapes, the TCP gateway, Plugin System v1's explicit instances/bindings, configuration validation, and the `shield::run` entry point.
- Coroutine-aware `call/sleep` inside handlers is on the current implementation path; timer callbacks and fork tasks likewise execute as coroutines (wrapped by `invoke_coroutine`) with the same suspension semantics as handlers. Real DB/Redis driver connections, UDP/KCP/WebSocket, ops snapshot, and the official optional modules are not blockers for the current minimal acceptance.
- Documents on schema/tooling, mappers, and entity/component are later drafts; unless the roadmap re-includes them, they must not expand the current runtime scope in reverse.

### Authoritative Contracts

| Document | Contents |
| --- | --- |
| Lua API Contract | the `shield.*` user API, service module shape, deleted legacy API list |
| Lua API Tests | the API test matrix; examples do not replace tests |
| Configuration Semantics | the YAML config schema (**authoritative source**), config validation, environment differences |
| Official Optional Module Contracts | boundaries, owners, and dependency directions of cluster/global/player/server/ops |
| [Official Optional Module Acceptance Matrix](optional-module-tests.md) | the optional module test matrix, guarding against reverse pollution of core |

### Core and First-Party Runtime Modules

| Document | Contents |
| --- | --- |
| Service Semantics | ServiceHandle, ServiceId, Service Registry, spawn, self, Lua service module, handler context, exit |
| Message Semantics | MessageEnvelope, MessagePayload, send, call, backpressure, QoS, timeouts, nested calls, coroutine scheduling, error handling |
| [Timer Semantics](runtime-timer.md) | timer API, time semantics, error semantics, sleep, fork, Task ID |
| [Lua VM Semantics](runtime-lua-vm.md) | Lua VM model, hot update strategy, Blue-Green replacement |
| [Network Semantics](runtime-network.md) | shield_net, shield_transport, gateway, `ClientContext`/`ClientRef`, Phase 1 TCP scope, deferred UDP/KCP/WebSocket |
| [Data Access Architecture](runtime-data.md) | zero data in core, plugin data interfaces, binding call shape, connection pools autonomous inside plugins |
| [Log Semantics](runtime-log.md) | log levels, structured format, context injection, rotation, audit logs |
| [Starter System](starter-system.md) | BootstrapContext, Starter order, ScriptStarter, deletion of the old DI/plugin design |
| [Bootstrap Flow](runtime-bootstrap.md) | startup order, shutdown order, timeouts, signal handling, graceful restart |
| Error Code Reference | a summary of all runtime error codes: message, service, resource, database, Redis, network |
| [Security Semantics](runtime-security.md) | Lua sandbox, inter-service permissions, network security, sensitive data protection |

### Official Optional Modules

| Document | Contents |
| --- | --- |
| Cluster Semantics | optional `shield_cluster`: node discovery, remote routing, node heartbeats, legacy code handling |
| Global Data | optional `shield_global`: GlobalData, cross-process shared data, distributed locks (reentrancy, renewal), leaderboards, rate limiter, Pub/Sub |
| Player Lifecycle | optional `shield_player`: PlayerSession, PlayerManager, authentication, disconnect/reconnect, offline message caching, multi-device policy |
| [Server State](runtime-server.md) | optional `shield_server` (P0 landed): ServerManager process-level singleton, four-state state machine, maintenance mode, `shutdown(ms)` shutdown handover, watch state notification, runtime info |
| Ops Semantics | optional `shield_ops`: ops endpoints, metrics, health checks |

### Drafts and Business Patterns

| Document | Contents |
| --- | --- |
| Entities and Components | game entity abstraction, component-based design, the Entity/Component/Room pattern; not part of the current core or the Phase 1 minimal runtime contract |
| Game State Persistence and Rollback | game business storage patterns, in-memory snapshots, incremental logs, rollback, transactions, and disaster recovery design; not part of the current core or the Phase 1 minimal runtime contract |

## Suggested Implementation Order

Follow-up coding proceeds in the order below to avoid circular dependencies.

### M1. Base types and targets

- Establish `shield_base`.
- Split targets per the [CMake Refactoring Strategy](cmake-refactor.md).
- Define `Result<T>`, `Error`, `ByteBuffer`, `TimePoint`.
- Define `ServiceId`, `NodeId`, `ServiceAddress`.
- Add a public header forbidden-CAF check.

### M2. Service identity and registry

- Implement `ServiceHandle`.
- Implement `ServiceRegistry`.
- Support name reserve/publish/unregister/query.
- Support automatic cleanup on owner exit.
- Add name conflict, invalid name, and stale handle tests.

### M3. Envelope and payload

- Implement `MessageEnvelope`.
- Implement `MessagePayload`.
- Implement the LuaPack encoding interface.
- Support method, argc, args.
- Add encoding tests for nil, false, multiple return values, and ServiceHandle.

### M4. spawn/send/call

- Implement `spawn` ready/rollback semantics.
- Implement non-blocking `send`.
- Implement the `call` pending registry.
- Implement timeout and late-response discard.
- Add self-send and stale handle tests (the CAF actor mailbox is unbounded; there is no mailbox-full case).

### M5. Lua service lifecycle

- Implement the module table loader.
- Implement `on_init` / method dispatch / `on_exit`.
- Implement `shield.self`, `shield.sender`, `shield.names`.
- Delete legacy APIs per the Lua API Contract.
- Add init failure rollback, method_not_found, and handler_error tests.

### M6. timer, sleep, fork

- Implement `TimerId`.
- Implement `timer_once`, `timer`, `cancel_timer`.
- Implement handler coroutine-aware `sleep`.
- Implement `fork` and the task id; the task body executes as a coroutine (wrapped by `invoke_coroutine`).
- Add fixed-delay, timer error stop, and service exit auto-cancel tests.

### M7. net/gateway/plugins

- Sort out the `shield_transport` and `shield_net` boundaries.
- Sketch the gateway Lua callback.
- Wire in Plugin System v1's manifest scan, instances, bindings, and `register_lua` dispatch.
- Data access is exposed through plugin namespaces; real MySQL/Redis connections, subscription lifecycles, and stress verification proceed independently and do not block the minimal Lua runtime.

The ops snapshot and local diagnostics belong to the `shield_ops` optional module; unless `shield_ops` is explicitly enabled, they must not enter the minimal runtime acceptance.

### M8. Optional cluster/global/ops

- Keep cluster out of core.
- Reserve `node_epoch` for route keys.
- Add a node heartbeat state model for ops.
- Implement `shield_cluster` independently later, and let `shield_global` and `shield_ops` depend only on public snapshot/handle semantics.
