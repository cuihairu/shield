# Skynet Comparison

Shield draws on Skynet's service model, but the goal is neither to clone Skynet nor to grow into a general distributed framework.

For full runtime semantics see the [Runtime Semantics Decisions](runtime-semantics.md).

## Similarities

- Services are the basic unit of execution.
- Lua carries the main business logic.
- `send` means asynchronous message.
- `call` means request-response.
- Timers and a lightweight service lifecycle are core capabilities.

## Differences

| Dimension | Skynet | Shield refactoring target |
| --- | --- | --- |
| Runtime language | C + Lua | C++23 + Lua |
| Actor foundation | hand-written | CAF internal implementation |
| User API | `skynet.*` | `shield.*` |
| Configuration | Lua config | YAML declarative config |
| Platforms | Unix-like primarily | Windows / macOS / Linux |
| Current scope | single process + cluster capability | single node first, cluster as an official optional module |

## API Mapping

```lua
-- Skynet
skynet.send(address, "lua", msg_type, ...)
local response = skynet.call(address, "lua", msg_type, ...)

-- Shield target
shield.send("service_name", method, data)
local ok, response = shield.call_timeout(timeout_ms, "service_name", method, data)
```

```lua
-- Skynet
skynet.timeout(100, function() ... end)

-- Shield target
shield.timer_once(100, function() ... end)
```

## CAF and Skynet Semantics

Shield's goal is not to re-implement Skynet's underlying actor runtime, but to let CAF carry the machinery layer and then fill in Skynet-like service semantics.

| Layer | Skynet | CAF | Shield decision |
| --- | --- | --- | --- |
| actor scheduling | Skynet hand-written | CAF scheduler | use CAF |
| actor/message | Skynet service/message | CAF actor/message | use CAF, hide the handle |
| async send | `skynet.send` | `send` / `anon_send` | wrap as `shield.send` |
| sync call | `skynet.call` + Lua coroutine yield | `request` + continuation | wrap as coroutine-aware `shield.call` |
| timer | `skynet.timeout` | `schedule` / delayed send | wrap as `shield.timer_once` / `shield.timer` |
| service name | handle/name service | actor handle / registry mechanism | Shield maintains its own service registry |
| Lua service | Lua script service | CAF does not handle Lua | implemented by `shield_lua` |
| remote actor | harbor/cluster | middleman publish/connect | not in core; belongs to `shield_cluster` |


## ID and Cluster Address Differences

Skynet's actor address can use the compact model of a high-bit cluster/harbor id and a low-bit local handle. Shield keeps this idea but does not make the bit layout part of the public `ServiceId` contract.

Shield decision:

```cpp
using ServiceId = uint64_t; // local service id
using NodeId = uint32_t;    // node id

struct ServiceAddress {
  NodeId node;
  ServiceId id;
};
```

Different nodes may have identical local `ServiceId`s; the global routing identity is determined by `{node_id, node_epoch, service_id}`. This avoids welding the public API to a fixed bit layout and also handles stale messages mis-delivered after a node restarts.

## Key Difference

CAF covers machinery; it does not cover Skynet's Lua service experience.

The most critical difference is `call`:

```text
Skynet call:
  send request → session binds the current Lua coroutine → yield → response arrives → resume

CAF request:
  send request → continuation / await / receive → C++ actor processes the response

Shield call:
  uses the CAF request mechanism
  but exposes Skynet-like coroutine suspension semantics to Lua
```

Therefore `shield.call` must neither directly expose the CAF request nor do a thread-blocking wait inside Lua. The correct design is to suspend the current Lua coroutine while keeping the actor runtime free to keep scheduling other messages.

## Semantics CAF Covers vs. What Shield Adds

CAF handles the actor machinery (spawn/quit, send/request, schedule, scheduler); Shield exposes game-server semantics on top of it. CAF does not directly provide Skynet-style Lua runtime semantics; Shield must add:

| Shield semantics | Why CAF does not provide it directly |
| --- | --- |
| service name registry | CAF primarily operates on actor handles, not game service names |
| Lua coroutine pending/resume | the CAF request knows nothing about Lua coroutines |
| the synchronous style of `shield.call` | CAF is a C++ continuation/request mechanism |
| Lua service lifecycle | CAF knows nothing about Lua scripts and init/message/exit |
| Lua table payloads | CAF leans toward the C++ type system |
| `uniqueservice`/`queryservice`-style semantics | this is Shield's service model, not CAF machinery |

No CAF exposure: Lua and user-side C++ extensions should not include CAF headers or manipulate CAF actor handles directly. For further discussion of CAF as an implementation detail see [Core Design Principles](architecture-core-concepts.md).

> This section was merged from the former `caf-mapping.md` (deleted).

## What Shield Does Not Cover

- The Skynet harbor cluster model does not directly enter core; multi-node capability belongs to `shield_cluster`.
- The snax framework.
- Shared-data mechanisms such as sharedata / stm.
- A built-in service discovery framework.
- Prometheus / health check / plugin system as core.

The current refactoring prioritizes making single-node services, messages, networking, and the Lua API thin and stable, then adds multi-node capability in `shield_cluster`.

## References

- Skynet source: https://github.com/cloudwu/skynet/blob/master/lualib/skynet.lua
- CAF `event_based_actor`: https://www.actor-framework.org/static/doxygen/0.18.7/classcaf_1_1event__based__actor
- CAF requester/request: https://www.actor-framework.org/static/doxygen/0.18.7/classcaf_1_1mixin_1_1requester
