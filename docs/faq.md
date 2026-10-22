# FAQ

## What is the current state of Shield?

The refactoring design phase is closed out: roadmap Phases 0-5 and the official optional module P0 have landed. The documentation describes target boundaries and the API direction.

## Is Shield a distributed framework?

`shield_core` is not a distributed framework. The refactored core focuses on services, messages, timers, and Lua coroutine semantics.

Shield can have an official `shield_cluster` optional module for multi-process / multi-machine communication, node heartbeats, remote routing cache, and optional service discovery. But it stays out of core and is not part of the minimal run path.

## Why were DI/IoC, plugins, Prometheus, and health checks moved out?

These capabilities would grow the core's surface and pull the project away from a Skynet-style lightweight runtime. The current priority is to make services, messages, networking, timers, and the Lua API stable; when needed, they can be attached as official optional modules or standalone extensions.

## Is HTTP still supported?

Phase 1 only freezes TCP sessions and basic transport framing. UDP, KCP, and WebSocket are later extension targets, not blockers for the first minimal acceptance.

HTTP does not enter the first version of `shield_core` and is not the default transport for the business gateway. The first version only allows `shield_ops` to provide HTTP management endpoints when explicitly enabled, such as `/ops/health`, `/ops/status`, and `/ops/metrics`. A business HTTP server, REST router, middleware chain, and web framework integration are all deferred to a standalone transport extension or left to the application layer to implement.

## Can `examples/hello_world/` be run directly?

The minimal path is already verifiable today: `include/shield/shield.hpp`, `shield::run(argc, argv)`, the CLI/config smoke tests, the default Phase 1 configuration, the `examples/hello_world` build-and-start path, and the Lua business message acceptance have all landed. Real multi-node scale verification and UDP/KCP/WebSocket plus real backend stress verification continue per the roadmap.

## What is the relationship to Skynet?

Shield draws on Skynet's service, message, and Lua-first ideas, but uses C++23 and CAF as its internal implementation foundation. Shield does not copy Skynet mechanisms such as harbor, snax, or sharedata.

## What about `/health`, `/status`, and Prometheus from the old docs?

These are not part of the current core. `/ops/health`, `/ops/status`, and `/ops/metrics` belong to the official `shield_ops` optional module; the Prometheus exporter is also an optional capability of `shield_ops`. When `shield_ops` is disabled, the runtime exposes no built-in HTTP management endpoints at all.
