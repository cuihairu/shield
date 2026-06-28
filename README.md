# Shield

[![C++23](https://img.shields.io/badge/C++-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Lua 5.4](https://img.shields.io/badge/Lua-5.4-blue.svg)](https://www.lua.org/)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)

Shield is being redesigned as a **single-node-first, Skynet-inspired,
actor-based, Lua-first game server runtime**.

This repository is currently in the **refactor design stage**. The documents in
this repository describe the target architecture and API direction, not a stable
released implementation.

## Target Positioning

- C++ owns runtime infrastructure: actor scheduling, networking, Lua binding,
  timers, configuration, logging, and the plugin host.
- Lua owns game logic: each service is a Lua script using the `shield.*` API.
- CAF remains an internal actor transport foundation.
- Shield exposes game-server semantics instead of CAF details.
- The minimal deployment path is a single-node runtime.
- Cluster support is an official optional extension outside the
  `shield_core` boundary.

## Non-Goals

Shield core does not provide:

- Distributed orchestration or cluster management in the core path.
- DI/IoC containers or annotation-based assembly.
- ORM or enterprise data frameworks.
- Cross-service or lifecycle event bus abstractions separate from actor
  messages.
- Middleware chains as a framework policy layer.
- Prometheus, health-check registries, or plugin systems as core runtime
  features.

These capabilities may be built by users or evaluated later as optional
extensions. They are not part of the current core contract.

`shield.event` is a deliberately small Lua helper for decoupling modules inside
one service VM. It is not a mailbox, not serialized, not visible across
services or VMs, and not used for application lifecycle events.

## Core Boundary

`shield_core` is intentionally narrow. It only owns the actor/service semantics
wrapped around CAF:

| Core piece | Responsibility |
| --- | --- |
| service lifecycle | create, name, stop, and inspect services |
| message semantics | `send` / `call` routing and timeout behavior |
| coroutine scheduling | suspend and resume Lua-facing calls without blocking runtime threads |
| timer semantics | timeout primitives attached to service execution |
| CAF adapter | keep CAF hidden behind Shield service handles |

First-party runtime modules are built around that core, but are not part of the
core semantics:

| Module | Responsibility |
| --- | --- |
| `shield_base` | Shared value types such as Result, Error, ByteBuffer, time, and IDs |
| `shield_lua` | Lua VM management and `shield.*` bindings |
| `shield_net` | Client connections (TCP in Phase 1; UDP/KCP/WebSocket deferred), Session management |
| `shield_transport` | Optional byte-stream adaptation such as framing or encryption |
| `shield_plugin` | Plugin manifest/catalog, instances, bindings, C ABI, and plugin Lua registration |
| `shield_config` | YAML configuration loading |
| `shield_log` | Runtime logging |
| `shield_bootstrap` | Compose selected modules into a runnable server |

Optional official extensions live outside the minimal main path:

| Module | Responsibility |
| --- | --- |
| `shield_cluster` | Cross-process/machine communication, service discovery, node heartbeat |
| `shield_global` | Distributed data, locks, queue/rank/rate limiting |
| `shield_player` | PlayerSession, PlayerRef, login/logout, reconnect, player-ready lifecycle |
| `shield_server` | Server state, maintenance mode, shutdown coordination |
| `shield_ops` | Diagnostics, metrics, console, profile |

## Target Lua Shape

```lua
local M = {}

function M.on_init()
    shield.log.info("echo started")
end

function M.on_shutdown(ctx)
    shield.log.info("echo draining: " .. ctx.reason)
end

function M.ping()
    local src = shield.sender()
    shield.send(src, "pong", { time = shield.now() })
end

return M
```

Player/avatar/client lifecycle is not a core service hook. When enabled,
`shield_player` owns `PlayerSession`, `PlayerRef`, player-ready state, login,
disconnect, reconnect, logout, and save hooks. `avatar` or `character` remains
game data attached to a player session, not a Shield core object.

The current source tree still contains modules from the previous broader
architecture. During the refactor, documentation should describe the target
boundary first; implementation work should then remove, merge, or demote modules
that no longer belong to `shield_core` or first-party runtime modules.

## Documentation

Authoritative design contracts:

- [Architecture](docs/architecture.md): module boundary, dependency direction,
  object ownership, and removed legacy directions.
- [Lua API Contract](docs/lua-api.md): target `shield.*` user API.
- [Runtime Semantics](docs/runtime-semantics.md): topic index and implementation
  order.
- [Configuration Semantics](docs/runtime-config.md): core configuration schema.
- [Optional Module Contracts](docs/optional-modules.md): official optional module
  boundaries.
- [Roadmap](docs/roadmap.md): staged refactor plan and current scope.
- [Decision Log](docs/open-decisions.md): closed design decisions and any newly
  discovered open questions that must be synchronized with authority docs.

Supplemental documents such as CMake refactor notes, ops design, schema drafts,
and historical refactor summaries are under `docs/`. If they conflict with the
authoritative contracts above, the authoritative contracts win.

## Current Status

The runtime is still converging toward the authoritative contracts above. Some
documented APIs are target contracts rather than implemented features; for
example, service `on_shutdown(ctx)` and local `shield.event` still require
runtime work before they are usable.

## Docker Build

The repository includes a multi-stage [Dockerfile](Dockerfile) for production
image builds. When building from a git checkout, pass the current commit hash as
a build argument so `shield --version` inside the image keeps the source
revision:

```bash
docker build \
  --build-arg SHIELD_GIT_COMMIT_HASH="$(git rev-parse HEAD)" \
  -t shield:latest .
```

If the build runs outside a git checkout, the image falls back to `Unknown` for
the embedded commit hash.
