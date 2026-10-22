# CMake Refactoring Strategy

> Status: **Historical record — the refactoring is complete; this is no longer a pending plan.**
>
> This document originally described the target splitting direction. The current `CMakeLists.txt` has converged on host targets such as `shield_base`/`shield_log`/`shield_config`/`shield_core`/`shield_plugin`/`shield_transport`/`shield_net`/`shield_lua`/`shield_bootstrap`/`shield`; data backends are provided as separate `plugins/*` shared libraries. The old discovery/metrics/health/DI modules and old plugin v0 have been deleted (see the Roadmap). This document is kept as a reference for target structure and dependency constraints.

This document defines the post-refactoring CMake target splitting direction.

## Goals

- `shield_core` compiles only service, message, timer, coroutine, and the CAF adapter.
- Lua, net, plugin, config, log, and bootstrap are all separate targets.
- discovery, metrics, health, DI, annotations, conditions, old plugin v0, and the old middleware modules are deleted outright; no compatibility targets are kept.
- Optional official modules are separate targets and are not linked into the minimal runtime by default.

## Target List

| Target | Contents | Allowed dependencies |
| --- | --- | --- |
| `shield_base` | Result, Error, ByteBuffer, time, id types | standard library |
| `shield_log` | logging facade and sinks | `shield_base` |
| `shield_config` | YAML loading, schema validation | `shield_base`, `shield_log`, `yaml-cpp` |
| `shield_core` | service registry, message router, timer, CAF adapter | `shield_base`, CAF |
| `shield_plugin` | manifest/catalog, instance, binding, C ABI host, `register_lua` dispatch | `shield_base`, `shield_log`, `shield_config`, Lua |
| `shield_transport` | protocol pipeline and RPC descriptor tables | `shield_base`, `shield_log` |
| `shield_net` | listener, session, connection management | `shield_base`, `shield_log`, `shield_config`, `shield_transport`, Asio/Beast |
| `shield_lua` | Lua VM, Lua API bindings, Lua service loader | `shield_base`, `shield_log`, `shield_config`, `shield_core`, `shield_net`, `shield_plugin`, Lua |
| `shield_bootstrap` | Starter, `shield::run` | selected runtime modules |
| `shield` | CLI/runtime executable | `shield_bootstrap` |

Optional:

| Target | Default | Notes |
| --- | --- | --- |
| `shield_cluster` | OFF | cross-process / multi-machine communication (heartbeat/transport/route learning/cross-node delivery) |
| `shield_global` | OFF | global KV, locks, leaderboards, queues, rate limiting, scheduler (P0 in-process `GlobalManager`; Redis backend left to Phase 2+) |
| `shield_player` | OFF | player setup state machine, multi-device adjudication, reconnect window, offline push (P0 landed) |
| `shield_server` | OFF | server state machine, maintenance mode, shutdown handover, ops snapshot (P0 landed) |
| `shield_ops` | OFF | diagnostics, metrics, health, console, profile |

## Forbidden Dependencies

`shield_core` must not depend on:

- Lua.
- Boost.Asio / Beast.
- Redis / MySQL / PostgreSQL.
- yaml-cpp.
- log implementation.
- HTTP, gateway, ops, cluster, global.
- discovery, metrics, health, DI, annotations, conditions, plugin.

`shield_lua` may depend on `shield_core`, but `shield_core` must not depend on `shield_lua` in reverse.

`shield_bootstrap` may compose modules, but modules must not depend on `shield_bootstrap` in reverse.

## Migration Steps

1. Create `src/shield_base` and move the base types.
2. Create `src/shield_core`, moving only the service handle, registry, message envelope, timer, and CAF adapter.
3. Remove log/config/CLI/net/data/script/gateway/protocol/serialization from the current `shield_core` target.
4. Create `shield_lua`, moving the Lua VM, Lua service loader, and Lua API bindings.
5. Create `shield_net` and `shield_transport`, merging the byte-stream and session code still needed from the old `protocol/http/gateway`.
6. Create `shield_plugin`, carrying the plugin manifest/catalog, instance lifecycle, bindings, C ABI, and `register_lua` dispatch; migrate data backends into separate plugin shared libraries.
7. Create `shield_bootstrap`, moving the Starter and `shield::run(argc, argv)`.
8. Delete the old module targets and sources: DI, annotations, conditions, events, discovery, old core metrics, old core health, old plugin v0, middleware.
9. Add a CMake dependency check forbidding `shield_core` from linking upper-layer libraries.
10. Add an include boundary check forbidding the public API from exposing CAF.

## Target CMake Shape

```cmake
add_library(shield_base STATIC ...)
add_library(shield_log STATIC ...)
add_library(shield_config STATIC ...)
add_library(shield_core STATIC ...)
add_library(shield_plugin STATIC ...)
add_library(shield_transport STATIC ...)
add_library(shield_net STATIC ...)
add_library(shield_lua STATIC ...)
add_library(shield_bootstrap STATIC ...)

target_link_libraries(shield_core
  PUBLIC shield_base
  PRIVATE CAF::core
)

target_link_libraries(shield_lua
  PUBLIC shield_base shield_core
  PRIVATE shield_log shield_config shield_net shield_plugin ${LUA_LIBRARIES}
)

target_link_libraries(shield_bootstrap
  PUBLIC shield_base
  PRIVATE shield_log shield_config shield_core shield_lua shield_net
)
```

## Checklist

CI should check at least:

- `shield_core` link libraries contain no Lua, Redis, Beast, yaml-cpp, or Boost.Log.
- `include/shield/core/**` includes no `lua.hpp`, `boost/asio`, or `yaml-cpp`.
- No `caf::actor` appears in the public API.
- Deleted old modules are not referenced by any target.
- The `shield` executable does not link the old `shield_extensions` bundle directly.
