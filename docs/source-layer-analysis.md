---
title: Source Layer Analysis
---

This page records an implementation-first layering of the source. The layer
count is a semantic boundary count, not a map of `src/` directories. Evidence
paths use the state at commit `e588c0e0a95800596aaefe3309d684944c20b15a`.

## Execution Roles

| Layer | Responsibility | State storage | Inbound channels | Outbound channels | Configuration | Lifecycle |
| --- | --- | --- | --- | --- | --- | --- |
| Process assembly | Validate configuration, construct and compensate every subsystem. | Process-global state owned by bootstrap. | CLI options; `Phase` starters. | Direct constructor calls; cleanup callbacks. | `config/*.yaml`; CLI overrides. | `initialize` to guarded teardown, then `shutdown`. |
| Configuration bus | Parse, merge, validate, and expose runtime YAML. | `Config::Impl` with reader/writer lock; process-global singleton. | YAML files; bootstrap. | `get*`, actor/runtime accessors, plugin subtree JSON. | Its own files are the configuration schema source. | Load, reset, and validate; no hot reload in this path. |
| Lua service domain | Own one actor-serialized VM and coroutine mailbox per service. | Service/VM/coroutine/timer/profile registries; process memory. | CAF `ServiceMessage`, `ClientIngress`, control and lifecycle messages; plugin post/resume hooks; console eval. | CAF calls/replies; gateway egress; plugin Lua calls; remote envelope via cluster. | `lua.*`, `actors[]`, `bootstrap.*`, `shutdown.*`. | Spawn/init, ready stashing, dispatch, drain, exit, force removal, manager shutdown. |
| Client gateway | Enforce network admission and route client RPCs to one bound target. | `TcpListener` session maps, `SessionBinding` on each session, gateway weak-session registry, route tables. | TCP/TLS packets; HTTP server requests; Lua bridge callbacks. | CAF ingress/control; session writes; `ClientEgress`. | Per-actor `network`, `network.protocol`, `network.tls`, `console`, `http`. | Listener/session start; actor handles connect, packet, disconnect; reverse-order stop. |
| Pluggable capability | Resolve, load, validate, start, and expose named provider instances. | Package/instance/binding registries; `dlopen` handles; plugin-owned pools. | YAML `plugins`; bootstrap; Lua namespace calls; dependency vtables. | Host API callbacks; dependency vtables; `register_lua`; Lua suspend/resume. | `manifest.yaml` plus instance `config_schema`; `plugins.instances/bindings`. | Scan→catalog→plan/resolve→load→create→start; reverse shutdown without premature `dlclose`. |
| Backend provider | Own a concrete external resource and expose its named capability. | Redis connection pools/streams, DB worker/pool state, codec state, plugin process memory. | Plugin host; Lua namespace; dependency vtables; HTTP/probe calls. | Redis, MySQL, PostgreSQL, SQLite, MongoDB, Prometheus, HTTP probes. | Manifest schema plus provider-specific config. | Create, start/connect, register Lua, shutdown/disconnect/drain; failed optional instance may degrade. |
| Optional runtime modules | Provide cross-service state, identity, state, and node coordination outside core. | `PlayerManager` and `ServerManager` mutex maps; `ClusterManager` node/route state; scheduler/queue memory; optional Redis. | Lua facade; bootstrap; transport callbacks. | Redis; CAF middleman; injected notify/stop callbacks; Lua task fire. | Module-owned `global`, `cluster`, `player`, `server_manager` sections. | Bootstrap start/stop; scheduler tick and shutdown timers. |
| Operations ingress | Read state and invoke explicitly enabled controls. | Embedded SPA and static filesystem state; profile/eval bookkeeping. | HTTP; Unix console sessions. | Config/plugin/service/global managers; Lua dispatch; JSON responses. | `http.*`, console tokens, static root. | Route registration during bootstrap; HTTP/console stop first. |
| Management client | Present polled operations snapshots. | `sessionStorage` bearer token; browser component state. | User interaction; HTTP JSON. | Fetch with bearer headers and cancellation/backoff. | Vite build config; runtime token entry. | Browser page lifecycle. |

## Implementation Map

### Foundation, Diagnostics, and Message Contracts

`shield_base` owns dependency-free ID, time, buffer, `Result`, and `Error`
primitives (`include/shield/base/base.hpp:5`, `include/shield/base/error.hpp:11`).
The CAF type registry lives beside it in `shield_core`
(`src/caf_initializer.cpp:1`); the service and client message shapes in
`include/shield/core/service_message.hpp:46` are the local contract, while
`include/shield/cluster/cluster_messages.hpp:22` adds the serializable wire
shapes. Logging is a sink facade with console/file/rotating implementations
(`include/shield/log/logger.hpp:14`, `include/shield/log/sinks.hpp:13`);
Crashpad arms a restartable handler only when enabled, writes local dumps, and
warns that upload is a placeholder (`src/crash/crash.cpp:31`, `src/crash/crash.cpp:82`).
These are shared contracts, not user-facing service layers.

### Process Assembly and Configuration Bus

`Bootstrap` is the composition root. It initializes logging, loads YAML, runs
`PRE_INIT`/`POST_CONFIG` starters, validates compiled-in module availability,
starts plugins, then constructs optional modules, CAF, Lua services, listeners,
console, and HTTP in order (`src/bootstrap/bootstrap.cpp:402`,
`src/bootstrap/bootstrap.cpp:508`). Any partial failure unwinds already started
listeners, servers, global state, transports, services, console, and plugin
context (`src/bootstrap/bootstrap.cpp:306`). Shutdown runs the observable
components first, drains Lua, then reverses module/plugin/resource ownership
(`src/bootstrap/bootstrap.cpp:1638`).

The configuration bus is not merely a file reader: `Config` holds a locked YAML
root and flattened values (`include/shield/config/config.hpp:14`), file
initialization lives at `src/config/config.cpp:1111`, and
`validate_runtime_config` gates optional module sections before consumers run
(`src/config/config.cpp:1115`). Module parsers keep their schemas beside their
implementations, for example `GlobalConfig::from_global_config` at
`src/global/global_manager.cpp:239` and `PlayerConfig::from_global_config` at
`src/player/player_manager.cpp:36`.

### Lua Service Domain

The Lua layer is the application execution domain. `LuaRuntime` owns VM policy
and service manager construction (`src/lua/lua_runtime.cpp:1`), while
`LuaServiceManager` serializes dispatch per service, implements
send/call/session completion, drain/exit, timers, async plugin completion,
naming, profiling, and forced teardown (`src/lua/lua_service.cpp:2884`,
`src/lua/lua_service.cpp:3397`). The `shield.*` API is the user contract:
service/message, timer/task, config/log, client, HTTP server, crypto, plugin
introspection, and optional module facades are registered in one composition
point (`src/lua/lua_api.cpp:5145`). Gateway and HTTP bridges translate their
channels into actor tasks rather than letting network threads touch a VM
(`src/lua/lua_gateway_bridge.cpp:46`, `src/lua/lua_http_bridge.cpp:80`).

### Client Gateway

`TcpListener` owns accept, connection/IP limits, blocklist, TLS handshake, rate
limits, frame-size limits, session registry, and metrics
(`include/shield/net/listener.hpp:76`). `TcpSession` stores the single-target
`SessionBinding`, monotonic epoch, queued output, token bucket, and typed
send controls (`include/shield/net/session.hpp:107`,
`include/shield/net/session.hpp:131`). The transport pipeline parses an
envelope, resolves header/body route metadata, optionally decodes through a
codec, and applies route policy (`src/transport/protocol.cpp:1420`,
`src/transport/protocol.cpp:1622`). The gateway bridge then validates direction,
auth, and route before sending one typed ingress to the session's already bound
target; it never routes by body or actor address
(`src/lua/lua_gateway_bridge.cpp:81`). The separate gateway actor is the only
mutator for binding replacement, close, and server-to-client egress
(`include/shield/lua/gateway_actor.hpp:1`). HTTP is a separate request/response
ingress: `HttpServer` dispatches, while ops and Lua-HTTP bridges own their
respective route semantics (`include/shield/net/http_server.hpp:31`,
`src/lua/lua_http_bridge.cpp:80`).

### Pluggable Capability and Backend Providers

`PluginHost` performs scan, catalog, plan/resolve, load, create, start, Lua
registration, and reverse shutdown (`src/plugin/plugin_host.cpp:1066`,
`src/plugin/plugin_host.cpp:1118`). A package is described by a manifest; the
host validates ABI identity and instance configuration before native code is
used (`include/shield/plugin/abi.h:33`, `src/plugin/plugin_host.cpp:266`,
`src/plugin/plugin_host.cpp:289`). `shield_host_api_v1` gives an instance only
its config, declared dependencies, Lua path/registration, and asynchronous
resume primitives (`include/shield/plugin/host_api.h:26`). The provider layer is
kept separate because provider packages own external sockets and drivers: Redis
driver/cache/queue/leaderboard use shared pools or controlled fallbacks
(`plugins/cache.redis/shield_cache_redis.cpp:969`,
`plugins/queue.redis/shield_queue_redis.cpp:1058`), while SQL/document packages
own worker pools and transaction lanes
(`plugins/mysql/shield_db_mysql.cpp:1796`,
`plugins/mysql/shield_db_mysql.cpp:2042`).

### Optional Runtime Modules

`GlobalManager` delegates KV/cache, lock, and rank domains through independent
backends selected as process memory or Redis (`src/global/global_manager.cpp:319`),
and keeps queues, scheduler, and rate limiting in process
(`src/global/global_manager.cpp:1196`, `src/global/global_manager.cpp:1343`).
Cluster transport publishes/dials peers and carries authenticated hello,
heartbeat, route snapshots, and envelopes over CAF middleman
(`include/shield/cluster/cluster_transport.hpp:3`,
`include/shield/cluster/cluster_messages.hpp:22`); discovery registers and
scans nodes through Redis (`include/shield/cluster/node_discovery.hpp:87`).
`PlayerManager` is deliberately only a local admission/reconnect/device index,
not an actor or persistence system (`include/shield/player/player_manager.hpp:79`,
`src/player/player_manager.cpp:102`). `ServerManager` owns the legal state
transition table and watcher/stop callbacks
(`include/shield/server/server_manager.hpp:20`, `src/server/server_manager.cpp:60`).

### Operations Ingress and Management Client

`OpsHttpHandler` maps health, status, metrics, services, plugins, config, SPA,
opt-in eval, and opt-in profile routes into runtime managers
(`src/console/ops_http_handler.cpp:805`). The Unix console dispatcher exposes
root and Lua commands from bootstrap construction
(`src/console/root_commands.cpp:25`, `src/console/lua_commands.cpp:24`). The
browser application is another boundary: it authenticates with a session-scoped
bearer token (`console/src/services/api.ts:14`), polls with cancellation, race
protection, and exponential backoff (`console/src/services/polling.ts:30`), and
only renders snapshots in pages such as the dashboard
(`console/src/pages/Dashboard.tsx:1`).

## Why These Boundaries Exist

1. **Bootstrap is the only ordered composition point.** The failure path
   enumerates every already-started subsystem because ownership and rollback
   are here, not in each subsystem (`src/bootstrap/bootstrap.cpp:306`).
2. **Configuration precedes consumers.** Optional module sections are rejected
   before those modules start, preventing silently ignored YAML
   (`src/config/config.cpp:1115`).
3. **The CAF message contract isolates Lua from callers.** Services receive
   typed `ServiceMessage`, `ClientIngress`, lifecycle, and response values, so
   neither network threads nor plugins manipulate Lua states directly
   (`include/shield/core/service_message.hpp:46`,
   `src/lua/lua_gateway_bridge.cpp:120`).
4. **One VM thread owns one service.** Spawn, dispatch, coroutine suspension,
   and forced teardown are serialized through the service manager, preventing
   concurrent Lua state mutation (`src/lua/lua_service.cpp:2884`,
   `src/lua/lua_service.cpp:3495`).
5. **Network admission and Lua routing are split at the session binding.** The
   session owns target/player/epoch while the gateway actor alone applies
   binding changes (`include/shield/net/session.hpp:107`,
   `include/shield/lua/gateway_actor.hpp:7`).
6. **Route policy is header-first.** Envelope parsing and descriptor-derived
   route tables run before ingress; the bridge rejects unknown, wrong-direction,
   and unauthenticated routes without decoding business identity
   (`src/transport/protocol.cpp:1533`, `src/lua/lua_gateway_bridge.cpp:85`).
7. **Plugins are named ABI instances, not linked host modules.** ABI/version/
   package identity and manifest schema are checked before `create`, and host
   access is restricted to declared dependencies (`include/shield/plugin/abi.h:33`,
   `src/plugin/plugin_host.cpp:289`).
8. **Providers own resource pools.** Redis packages use the shared driver or an
   explicit internal fallback, while MySQL/PostgreSQL/SQLite/MongoDB own worker,
   transaction, and pool drain semantics (`plugins/cache.redis/shield_cache_redis.cpp:969`,
   `plugins/mysql/shield_db_mysql.cpp:2042`).
9. **Global state is domain-partitioned, not one blob.** Data, lock, and rank
   each select their backend while queues/scheduler/rate limits remain
   in-process (`src/global/global_manager.cpp:319`,
   `src/global/global_manager.cpp:1196`).
10. **Cross-node transport is separate from route ownership.** Discovery only
    supplies addresses, transport carries authenticated snapshots/envelopes, and
    `ClusterManager` owns liveness/routes
    (`include/shield/cluster/node_discovery.hpp:31`,
    `include/shield/cluster/cluster_messages.hpp:80`).
11. **Player identity is an index, not an actor.** It decides admission,
    reconnect, and duplicate login, then points to the player's service actor;
    business state stays in that service
    (`src/player/player_manager.cpp:102`, `src/player/player_manager.cpp:229`).
12. **Server state has one legal transition surface.** A static transition
    matrix, watchers, and injected stop callback keep shutdown coordination out
    of Lua and out of unrelated modules (`src/server/server_manager.cpp:60`).
13. **Observability is read-only by default.** HTTP/console expose snapshots;
    eval/profile require separate opt-ins and tokens
    (`src/console/ops_http_handler.cpp:921`,
    `src/console/ops_http_handler.cpp:942`).
14. **The management application trusts only authenticated envelopes.** It
    stores the bearer token in `sessionStorage`, handles 401 separately, and
    never overwrites newer poll data (`console/src/services/api.ts:14`,
    `console/src/services/polling.ts:30`).
15. **Build boundaries preserve the same semantics.** Core/transport/net remain
    independent static libraries; optional modules are conditional; plugins are
    runtime shared libraries rather than host dependencies
    (`CMakeLists.txt:345`, `CMakeLists.txt:424`, `CMakeLists.txt:506`).

## Related Documents

- [Source Layer Glossary](glossary.md)
- [源码分层术语表](/zh/glossary)
- [Core Design Principles](architecture-core-concepts.md)
- [核心设计理念](/zh/architecture-core-concepts)
