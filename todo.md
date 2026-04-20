# Shield Boundary Refactor TODO

This file tracks the boundary-focused refactor work for the current codebase.
It is intentionally execution-oriented and narrower than the long-form
architecture notes under `docs/`.

## Goals

- Shrink `core::ApplicationContext` back toward a lifecycle-oriented kernel.
- Separate gateway transport concerns from request dispatch and actor/script
  orchestration.
- Keep `actor` depending on discovery abstractions, not concrete provider
  selection logic.
- Make plugin dependency declarations enforceable at runtime.
- Preserve current behavior while tightening module seams.

## Boundary Rules

- `core` defines lifecycle and startup orchestration, not business policy.
- `gateway` accepts connections and hands requests to application-level
  dispatchers; it should not contain embedded business routes long-term.
- `actor` provides orchestration and routing for actors; it should not choose
  infra implementations directly.
- `script` is a runtime boundary for Lua execution, not a backdoor into the
  whole application container.
- `discovery` is accessed via `IServiceDiscovery`; concrete backends stay in
  infra and composition code.

## Phase 1: Immediate Fixes

- [x] Record the boundary plan in a repo-root task file.
- [x] Enforce plugin dependency order in `PluginManager`.
- [x] Extract gateway request dispatch logic out of `GatewayService`.
- [ ] Add focused tests for plugin dependency ordering.
- [ ] Add focused tests for gateway dispatcher behavior.

## Phase 2: Kernel and Composition

- [ ] Split `ApplicationContext` responsibilities into:
  - lifecycle registry
  - bean/service lookup facade
  - [x] config reload binder
  - [x] plugin host
- [ ] Introduce a dedicated composition root for infrastructure assembly.
- [x] Move discovery backend selection out of `ActorStarter`.
- [ ] Replace direct `ConfigManager::instance()` calls inside services with
  injected config access where practical.

## Phase 3: Gateway Boundary

- [ ] Introduce a gateway application port for request/message dispatch.
- [ ] Move hard-coded HTTP routes out of `GatewayService`.
- [ ] Move Lua actor creation strategy behind an explicit adapter/factory.
- [ ] Isolate session lifecycle storage from protocol handlers.

## Phase 4: Actor and Script Runtime

- [ ] Make `DistributedActorSystem` fully self-consistent as a service.
- [ ] Remove dual-mode initialization paths that silently run without required
  dependencies.
- [ ] Define a stable script runtime API that does not expose the full
  application container.
- [ ] Reduce `lua_ioc_bridge` reach into unrelated framework concerns.

## Acceptance Criteria

- `GatewayService` no longer owns route policy and actor message dispatch
  details directly.
- Plugin load order respects declared dependencies and fails fast on cycles or
  missing dependencies.
- New modules can be added without expanding `ApplicationContext` further.
- Infra implementation choice is localized to composition code.
