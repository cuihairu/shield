# Shield

Shield is being redesigned as a **single-node, Skynet-inspired, actor-based,
Lua-first game server runtime**.

This repository is currently in the **refactor design stage**. The documents in
this repository describe the target architecture and API direction, not a stable
released implementation.

## Target Positioning

- C++ owns runtime infrastructure: actor scheduling, networking, Lua binding,
  timers, configuration, logging, and low-level data access.
- Lua owns game logic: each service is a Lua script using the `shield.*` API.
- CAF remains an internal actor transport foundation.
- Shield exposes game-server semantics instead of CAF details.
- The first refactor target is a single-node runtime. Multi-node orchestration is
  outside the core boundary.

## Non-Goals

Shield core does not provide:

- Distributed orchestration or cluster management.
- DI/IoC containers or annotation-based assembly.
- ORM or enterprise data frameworks.
- Event bus abstractions separate from actor messages.
- Middleware chains as a framework policy layer.
- Prometheus, health-check registries, or plugin systems as core runtime
  features.

These capabilities may be built by users or evaluated later as optional
extensions, but they are not part of the current refactor contract.

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
| `shield_net` | TCP / UDP / WebSocket I/O and connection management |
| `shield_ipc` | Same-host process communication and heartbeat state, outside core |
| `shield_transport` | Optional byte-stream adaptation such as framing or encryption |
| `shield_data` | Raw DB / Redis access, without ORM policy |
| `shield_config` | YAML configuration loading |
| `shield_log` | Runtime logging |
| `shield_bootstrap` | Compose selected modules into a runnable server |

## Target Lua Shape

```lua
local M = {}

function M.on_init()
    shield.log.info("echo started")
end

function M.ping()
    local src = shield.sender()
    shield.send(src, "pong", { time = shield.now() })
end

return M
```

The current source tree still contains modules from the previous broader
architecture. During the refactor, documentation should describe the target
boundary first; implementation work should then remove, merge, or demote modules
that no longer belong to `shield_core` or first-party runtime modules.

## Documentation

- [Architecture Definition](ARCHITECTURE.md)
- [Docs Home](docs/index.md)
- [Architecture Design](docs/architecture.md)
- [Core Concepts](docs/architecture-core-concepts.md)
- [Runtime Semantics](docs/runtime-semantics.md)
- [Ops Design](docs/ops.md)
- [Roadmap](docs/roadmap.md)
- [API Notes](docs/api.md)

## Current Status

Design is not finalized. Treat examples under `examples/hello_world/` as target
API sketches until the corresponding runtime entrypoint and Lua bindings are
implemented.
