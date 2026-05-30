# Shield Refactor TODO

This file tracks the current refactor plan for Shield.
It replaces the old boundary note and focuses on one goal:

Build a Skynet-inspired, actor-based, Lua-first game server runtime that is
more out-of-the-box than Skynet, while keeping CAF as the actor transport
foundation.

## Target Positioning

- Use Skynet as the design reference for service runtime semantics.
- Use CAF to cover actor communication and distribution semantics.
- Keep Lua as the primary business scripting language.
- Make HTTP, WebSocket, TCP, UDP, discovery, metrics, and tooling available
  by default.
- Keep the framework cross-platform: Windows, macOS, Linux.
- Reduce framework drift by separating core runtime from optional extensions.

## Core Principles

- `core` owns lifecycle, composition, service startup, and shutdown.
- `actor` owns service semantics, actor routing, and distributed actor access.
- `script` owns Lua runtime and Lua service adapters.
- `protocol` owns transport adaptation, not business policy.
- `gateway` owns connection acceptance and session orchestration.
- `discovery` stays behind an abstraction layer.
- `database`, `metrics`, `plugin`, and similar features remain optional unless
  required for the runtime bootstrap path.

## Phase 1: Boundary Reset

- [x] Define the Shield core boundary in docs and keep it stable.
- [x] Split core runtime from optional infrastructure modules.
- [x] Reduce `ApplicationContext` responsibilities to lifecycle and lookup.
- [x] Remove framework policy from service startup code.
- [x] Identify and tag modules that belong to core, starter, or plugin.
- [x] Decide which existing modules are outside the runtime boundary and mark
      them optional.

## Phase 2: Skynet Semantics Layer

- [x] Define a Shield service model aligned with Skynet concepts.
- [x] Provide `send` and `call` semantics over CAF.
- [x] Add timer and timeout primitives for services.
- [x] Add `sleep`, `fork`, and deferred execution primitives.
- [x] Add service naming, lookup, and lifecycle helpers.
- [x] Add a minimal debug/console path for runtime inspection.
- [x] Define cluster call and proxy semantics for remote service access.

## Phase 3: Lua Runtime

- [x] Provide a default Lua service base class.
- [x] Standardize Lua service entry points and message dispatch.
- [x] Expose runtime helpers to Lua in a Skynet-like shape where practical.
- [x] Keep Lua API stable for business logic authors.
- [x] Separate Lua business logic from framework internals.
- [x] Improve hot reload behavior and define its supported scope.

## Phase 4: Network Runtime

- [x] Keep the high-performance HTTP server as a default capability.
- [x] Expose WebSocket as a first-class protocol path.
- [x] Keep TCP and UDP as standard transport adapters.
- [x] Unify protocol request and response models.
- [x] Add request routing and middleware-style hooks where needed.
- [x] Add gateway templates for game login, session, and message dispatch.

## Phase 5: Out-of-the-Box Packaging

- [x] Provide ready-to-run templates for single-node and multi-node servers.
- [x] Provide a default game server template with Lua business scripts.
- [x] Provide a gateway + logic + storage reference layout.
- [x] Provide a minimal deployment path with one command build/run guidance.
- [x] Make Windows, macOS, and Linux build instructions explicit and current.
- [x] Reduce the amount of manual wiring required to start a new project.

## Phase 6: Observability and Operations

- [ ] Keep health checks available by default.
- [ ] Keep Prometheus metrics as a standard operational feature.
- [ ] Add runtime diagnostics for actors, services, and network status.
- [ ] Add configuration reload rules and document their scope.
- [ ] Add logging conventions for service, protocol, and runtime layers.

## Phase 7: Optional Extensions

- [ ] Keep database access abstractions optional unless required by a template.
- [ ] Keep plugin support isolated from the core runtime path.
- [ ] Keep advanced DI/IoC features from expanding the runtime surface.
- [ ] Keep advanced data access features out of the minimal bootstrap path.

## Documentation Plan

- [ ] Migrate docs to VitePress.
- [ ] Create a docs structure centered on runtime, Lua, network, and cluster.
- [ ] Add a Skynet comparison page.
- [ ] Add a CAF mapping page explaining what CAF covers and what Shield adds.
- [ ] Add a quickstart that boots a real server in the smallest possible steps.
- [ ] Add a template-based tutorial for a small multiplayer game backend.
- [ ] Add API pages for actor, script, protocol, discovery, gateway, and core.

## Acceptance Criteria

- Shield has a clear runtime boundary and does not drift into a generic
  enterprise framework.
- CAF is the actor transport foundation, but Shield exposes a higher-level
  service model for game server authors.
- Lua business scripts can be written with minimal framework ceremony.
- HTTP, WebSocket, TCP, UDP, discovery, and metrics work out of the box.
- Windows, macOS, and Linux are all documented and supported.
- The docs explain how Shield differs from Skynet instead of hiding the
  relationship.

