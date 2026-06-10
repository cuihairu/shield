# Shield Refactor Design TODO

This file tracks the refactor design work for Shield.

Current goal:

Build a Skynet-inspired, actor-based, Lua-first **single-node** game server
runtime. Keep CAF as the internal actor transport foundation, but expose a small
Shield service API to Lua users.

## Design Source of Truth

- `ARCHITECTURE.md` defines the target runtime boundary.
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

## Design Tasks

- [ ] Freeze the final `shield_core` semantic boundary.
- [ ] Freeze first-party module names and directory layout.
- [ ] Decide whether existing `gateway/` remains a directory or becomes a Lua
      template over `actor` + `net`.
- [ ] Decide how existing `protocol/` code is merged into `shield_net` or
      `shield_transport`.
- [ ] Decide whether `database/` and `data/` collapse into one raw data module.
- [ ] Define the final `shield.*` Lua API surface.
- [ ] Define the final C++ top-level include and `shield::run(argc, argv)`
      entrypoint.
- [ ] Mark old docs as design-only, rewritten, or removed.
- [ ] Align examples with implemented Lua bindings.
- [ ] Add CI checks for forbidden `shield_core` dependencies after the boundary is
      stable.

## Acceptance Criteria

- Documentation no longer claims the old Phase 1-7 architecture is complete.
- Documentation consistently describes Shield as a single-node runtime.
- Optional or removed features are clearly marked as non-core or deferred.
- The target API is described separately from the current implementation state.
- The codebase can be refactored module by module without changing the public
  design contract every time.
