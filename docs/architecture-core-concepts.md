# Core Design Principles

This document explains the post-refactoring design principles; the implementation has converged on them (see the Roadmap), and the landed status of individual items is annotated where it applies.

For concrete runtime semantics, API return values, IDs, registry, payload, timer, Lua VM, ops, and cluster reserved rules, see the [Runtime Semantics Decisions](runtime-semantics.md).

## 1. Single node first, cluster optional

Shield must first make the single-node game service runtime stable. Multi-node deployment, service discovery, cluster routing, and node orchestration do not enter `shield_core`.

`shield_cluster` is the official optional module for multi-process / multi-machine communication, node heartbeats, remote routing cache, and optional service discovery. It must reuse core's service handle, send/call, timeout, and error semantics; it must not stuff service discovery or cluster orchestration back into core.

The reason for this tradeoff is direct: Shield first makes the core paths — services, messages, networking, timers, hot Lua iteration — thin and stable, then adds cluster capability on clean boundaries, instead of continuing to expand into a generic enterprise framework.

## 2. Lua first

Game logic is written in Lua by default. C++ is for the runtime, networking, Lua bindings, low-level data access, and a small number of performance-sensitive extensions.

```lua
shield.send("room", "join", { player_id = "1001" })
local ok, result = shield.call("player", "get_info", { id = "1001" })
```

Lua business code should not be aware of CAF, the threading model, connection objects, or C++ container details.

## 3. CAF is an implementation detail

CAF handles the actor machinery: spawn, send, request, schedule, scheduling, and so on. Shield layers game-server semantics on top:

- Named services, not directly exposed CAF actor handles.
- `send` / `call` dual mode.
- Service lifecycle hooks.
- Timers and delayed tasks.
- Lua-friendly table data structures.

The user-facing API does not expose CAF headers.

The one-line principle:

```text
CAF covers the machinery; Shield adds the service semantics.
```

CAF can carry actor scheduling, mailboxes, spawn, send, request, timers, and remote actor transport. Shield still must implement the service name registry, Lua coroutine pending/resume, the Lua service lifecycle, and the `shield.*` API.


## 4. Small core

`shield_core` contains only the Actor/Service semantics:

- Service lifecycle.
- Service naming and lookup.
- `send` / `call`.
- `spawn` / `exit`.
- timer / timeout.
- coroutine pending/resume.
- CAF adapter.

Lua, network, transport, data, config, log, and bootstrap are all official runtime modules around `shield_core`, not core semantics themselves. `cluster`, `global`, `ops`, `player`, etc. are official optional modules.

DI, annotations, conditional wiring, plugins, Prometheus, health checks, service discovery, event buses, a standalone protocol layer, and similar pieces of the old architecture are all NOT core. If any must be kept, it needs an explicit non-core home and must not enter core or the default startup main path.

## 5. Messages are events

Actor messages are the only cross-service communication model. No separate event bus is designed to express the same concept.

`shield.event`, if it exists, can only be a synchronous observer utility inside the current Lua service / current Lua VM. It does not enter the mailbox, does not serialize payloads, does not cross service/VM/node boundaries, and is not used for application ready, shutdown, or service lifecycle orchestration.

Recommended pattern:

```lua
shield.send("chat", "broadcast", {
    channel = "world",
    text = "hello"
})
```

Use `shield.call` only when a return value is needed.

## 6. The gateway is the connection and session binding boundary

The Gateway runtime owns client connections and session binding management; it is not a generic Lua message dispatcher. The fixed chain for ordinary client RPC is:

```text
frame decode -> header.route_id
  -> Gateway lightweight routing table check
  -> session.target (AuthService or PlayerService)
  -> CAF ClientIngress
  -> target Service adapter: cached handler
  -> body decode
  -> concrete Lua RPC handler
```

Route information lives only in the wire header; the body only carries the business parameters of that RPC. The Gateway does not parse the target service from the route_id, does no multi-target routing, and does not decode ordinary business bodies.

The CAF behavior distinguishes internal message categories such as `ClientIngress`, ordinary service send/call, and lifecycle control; the Shield Service adapter maps the header `route_id` to Lua handlers cached in the target VM at startup.

All client messages go to the target service bound to the session (before login → AuthService, after login → PlayerService). Dynamic routing of room/scene/map is managed by PlayerService private state and does not participate at the Gateway layer.

## 7. Data access is provided by plugins

Data access does not enter `shield_core`, and is no longer carried by a separate `shield_data` module. Backend capabilities such as SQL, document stores, caches, queues, and leaderboards are provided by Plugin System v1; business Lua accesses them through plugin namespaces and binding logical names.

```lua
local db = shield.database.mysql("database.default")
local ok, rows = db:query("SELECT * FROM users WHERE id = ?", { id })

local q = shield.queue.redis("queue.events")
q:publish("chat.world", { text = "hello" })
```

Plugins own connection pooling and driver integration; core provides no ORM, generates no complex mappers, and manages no cross-service transactions.

## 8. Operations and debugging are separated

`shield_ops` is the official optional operations and debugging layer; it is not part of `shield_core`.

- `shield_core` owns semantics.
- `shield_ops` owns metrics, health, profile, diagnostics, and the console.
- `shield_ops` may read internal state, but must not depend on core semantics in reverse.
- Business code should not call `shield_ops` directly; it may only use it through management endpoints or the console.
