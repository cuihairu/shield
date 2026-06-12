# Shield Refactor Design TODO

This file tracks the refactor design work for Shield.

Current goal:

Build a Skynet-inspired, actor-based, Lua-first **single-node** game server
runtime. Keep CAF as the internal actor transport foundation, but expose a small
Shield service API to Lua users.

## Design Source of Truth

- `docs/architecture.md` defines the target runtime boundary.
- `examples/hello_world/` sketches the final user-facing API.
- `docs/` must describe the refactor target and clearly mark unfinished areas.
- Source headers, examples, and tests remain useful references, but the current
  implementation still contains pre-refactor modules.

## Core Principles

- `shield_core` owns only Actor/Service semantics.
- `shield_core` hides CAF behind service handles.
- `shield_core` owns service lifecycle, service registry, `send` / `call`,
  `spawn` / `exit`, timer semantics, and coroutine pending/resume.
- `shield_core` must not depend on Lua, network, data, config, or log modules.
- `shield_lua` exposes `shield_core` semantics to Lua.
- `shield_net` owns TCP / UDP / WebSocket I/O and connection management.
- `shield_transport` adapts byte streams before messages enter Lua gateway
  services.
- `shield_data` owns raw DB / Redis access without ORM policy.
- `shield_config`, `shield_log`, and `shield_bootstrap` are infrastructure
  modules around the core, not part of the core semantics.
- Gateway behavior is a Lua service pattern on top of `shield_core` +
  `shield_net`, not a separate middleware framework.

## Explicit Non-Goals

- No distributed orchestration in core.
- No service discovery framework in core.
- No DI/IoC container in core.
- No annotation or conditional assembly system.
- No event bus separate from actor messages.
- No middleware chain as framework policy.
- No Prometheus or health-check registry in core.
- No plugin system in core.
- No ORM.

## Decision Status

These items are no longer design-open unless their authority document changes.

| Topic | Decision | Remaining work |
| --- | --- | --- |
| `shield_core` boundary | Frozen by `docs/architecture.md` and `docs/runtime-semantics.md`. Core owns only service/message/timer/coroutine semantics around CAF. | Enforce with code movement and forbidden include checks. |
| First-party module names | Frozen as `shield_base`, `shield_core`, `shield_lua`, `shield_net`, `shield_transport`, `shield_data`, `shield_config`, `shield_log`, `shield_bootstrap`. | Decide physical source directory layout before large file moves. |
| Gateway | Gateway is a Lua service pattern over `shield_core` + `shield_net`, not a separate middleware framework. | Move current `src/gateway` code to Lua templates, `shield_net`, or legacy removal path. |
| Protocol code | No standalone `shield_protocol` in the current core path. Byte framing/codec/encryption belong to `shield_transport`; client session handling belongs to `shield_net`; schema protocol is deferred tooling. | Classify each current `src/protocol` file into transport, net, or deferred schema. |
| `database/` and `data/` | Collapse into one raw `shield_data` module. ORM, mapper, migration, and cross-service transaction policy are out of core. | Migrate or delete current `src/database` and `include/shield/database`. |
| Lua API | Frozen by `docs/lua-api.md` and `docs/lua-api-tests.md`. | Minimal single-node bindings now exist for lifecycle, spawn/exit, send/call, registry query/register/unregister/names, context, now, log and data unavailable errors; coroutine-aware scheduling, opaque handles and negative legacy tests remain. |
| C++ public entry | Target public entry is `include/shield/shield.hpp` with `shield::run(argc, argv)`. | Keep implementation and tests aligned with `docs/runtime-bootstrap.md`. |
| CLI contract | Frozen by `docs/runtime-bootstrap.md`. `--config` defaults to `config/app.yaml`, may repeat, and `--node-id` is cluster-only. | CLI smoke tests exist; extend when cluster is implemented. |
| Process lifecycle | `shield::run` owns help/version, signal handling, runtime loop and exit codes. | MSVC build validates the Windows entry path; signal behavior still needs interactive/system tests. |
| Physical directory layout | Short term keeps current physical directories; CMake target and include boundaries are the enforcement layer. | Delay mechanical `src/shield_*` moves until tests are stable. |
| Phase 1 network scope | Phase 1 freezes TCP session, `SessionHandle`, and basic transport framing only. | Mark UDP/KCP/WebSocket as deferred in tests and examples until implemented. |
| Optional module failure policy | Frozen by `docs/optional-modules.md`: optional config without enabled module fails startup; enabled modules fail fast by default. | Bootstrap validation now rejects disabled optional config sections; module-specific registries still need implementation. |
| Data API phase split | Phase 1 is `shield.db.query/query_one/execute` plus `shield.redis.get/set/del/exists/publish/subscribe`; transactions, prepare, pipeline, eval, sentinel/cluster are Phase 2+. | Lua bindings and mock-pool smoke tests exist; real DB/Redis drivers and subscription lifecycle remain. |
| Legacy compatibility window | No public compatibility shim. Legacy Lua deletion requires negative tests; legacy C++ target deletion requires examples/tests to stop referencing it. | Remove old targets and headers only after those gates pass. |
| Historical docs | `refactor-summary.md` and `refactor-complete.md` are historical snapshots, not current authority. | Continue marking or removing stale docs as they are found. |
| Examples | `examples/hello_world/` builds and starts through `shield::run`. | Add Lua business-message acceptance once bindings/runtime service startup are complete. |
| CI checks | Required after boundary stabilization. | `shield_core` forbidden include check and new public/core CAF leakage check exist; legacy `actor/service/gateway` headers still expose CAF and need migration before full-header enforcement. |

## Open Decisions

No foundation-level design decisions are currently open. New unresolved details should be recorded in `docs/open-decisions.md` before implementation.

## Acceptance Criteria

- Documentation no longer claims the old Phase 1-7 architecture is complete.
- Documentation consistently describes Shield as a single-node runtime.
- Optional or removed features are clearly marked as non-core or deferred.
- The target API is described separately from the current implementation state.
- The codebase can be refactored module by module without changing the public
  design contract every time.
