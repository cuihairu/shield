# CAF Mapping

Shield uses [CAF](https://github.com/actor-framework/actor-framework) (C++ Actor Framework) as its actor transport foundation. This page explains what CAF provides and what Shield adds on top.

## What CAF Provides

CAF is a modern C++ actor framework handling:

| Layer | CAF Feature |
|-------|-------------|
| **Actor lifecycle** | `caf::event_based_actor`, spawn, quit |
| **Message passing** | `send()`, `request()`, `anon_send()` |
| **Type system** | Typed actors, inspectable messages |
| **Serialization** | Built-in message inspection for networking |
| **Distribution** | `caf::io::middleman` for remote actors |
| **Scheduling** | Work-stealing scheduler |
| **Timeouts** | `after()` delays, `request()` timeouts |

## What Shield Adds

Shield wraps CAF with game-server-specific semantics:

### Service Layer (`shield/service/`)

| Shield API | CAF Equivalent | Purpose |
|------------|----------------|---------|
| `service::send(name, type, payload)` | `caf::anon_send(actor, type, payload)` | Fire-and-forget messaging by service name |
| `service::call(name, type, payload, timeout)` | `self->request(actor, timeout, type, payload)` | Sync request-response by service name |
| `service::query(name)` | `DistributedActorSystem::find_actor()` | Named service lookup |
| `service::uniqueservice(name)` | `query()` + spawn if missing | Singleton service guarantee |
| `service::name(handle, name)` | `register_actor()` | Service registration |
| `service::timeout(ms, cb)` | `self->schedule()` | Timer primitive |
| `service::fork(func, name)` | `caf_system.spawn()` | Create new actor |

### Service Context (`service::ServiceContext`)

CAF actors are anonymous — you need `self` and `actor_system` references. Shield provides a thread-local `ServiceContext` with RAII `Guard`:

```cpp
// Inside any message handler:
service::ServiceContext::Guard guard(svc_ctx);
svc_ctx.set_self(this);  // CAF actor pointer

// Now all shield::service::* free functions work
service::send("player_manager", "get_info", R"({"id":"123"})");
```

### Lua Integration (`shield/script/`)

Shield exposes the service API to Lua via `shield.*` global table, so game logic authors never touch CAF:

```lua
-- Lua script — no CAF knowledge required
local result = shield.call("player_manager", "get_info", { player_id = "123" })
shield.send("room_manager", "player_joined", { room_id = "lobby_1" })
```

### Gateway Layer (`shield/gateway/`)

Shield adds multi-protocol networking on top of CAF's actor system:

- TCP: `MasterReactor` → `SlaveReactor` → `Session` → `BinaryProtocol`
- HTTP: `BeastHttpServer` → `HttpRouter` → Lua actor
- WebSocket: `WebSocketProtocolHandler` → Lua actor
- UDP: `UdpReactor` → `UdpSession` → Lua actor

All protocols converge at `GatewayRequestDispatcher` → `MiddlewareChain` → Lua actor.

### Discovery (`shield/discovery/`)

CAF has `io::middleman` for remote actor connections. Shield adds service discovery backends:

- Static (development)
- Redis
- Nacos
- Consul
- Etcd

## Architecture Diagram

```
┌─────────────────────────────────────────────┐
│                 Lua Scripts                  │
│  (on_init / on_message / shield.* API)      │
├─────────────────────────────────────────────┤
│              Service Layer                   │
│  send / call / query / timeout / fork        │
├─────────────────────────────────────────────┤
│           ServiceContext (thread-local)       │
├─────────────────────────────────────────────┤
│              CAF Actor System                │
│  (spawn, send, request, schedule, inspect)   │
├─────────────────────────────────────────────┤
│         DistributedActorSystem              │
│  (registry, discovery, routing)              │
├─────────────────────────────────────────────┤
│           Gateway / Protocol                 │
│  TCP / UDP / HTTP / WebSocket                │
└─────────────────────────────────────────────┘
```

## Key Principle

> CAF handles actor mechanics. Shield handles game-server semantics.
>
> Lua scripts never see CAF. They see `shield.send`, `shield.call`, `shield.query`.
