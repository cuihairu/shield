# Skynet Comparison

Shield draws heavy inspiration from [cloudwu/skynet](https://github.com/cloudwu/skynet), but targets C++20 with CAF as the actor transport and Lua as the primary business scripting language.

## What Skynet Does

Skynet is a lightweight, C+Lua actor framework for online game servers. Its core model:

- Every service is an actor (C or Lua)
- Services communicate via `send` (async) and `call` (sync)
- Each actor has a unique handle (`:hex_address`)
- Lua scripts handle game logic; C handles performance-critical paths
- `skynet.fork`, `skynet.timeout`, `skynet.sleep` for concurrency primitives

## What Shield Does Differently

| Concept | Skynet | Shield |
|---------|--------|--------|
| **Language** | C + Lua | C++20 + Lua |
| **Actor Transport** | Custom C framework | CAF (C++ Actor Framework) |
| **Distribution** | Custom cluster protocol | CAF middleware + configurable discovery |
| **Networking** | Built-in TCP gateway | Boost.Asio/Beast TCP/UDP/HTTP/WS |
| **Scripting** | Lua 5.3+ | Lua 5.4+ via sol2 |
| **Configuration** | `.lua` config files | YAML with hot reload |
| **Metrics** | Custom | Prometheus via prometheus-cpp |
| **Build** | Make | CMake + vcpkg |
| **Platforms** | Linux/macOS | Windows + Linux + macOS |

## API Mapping

### Service Communication

```lua
-- Skynet
skynet.send(address, "lua", msg_type, ...)
local response = skynet.call(address, "lua", msg_type, ...)

-- Shield
shield.send("service_name", msg_type, data)
local response = shield.call("service_name", msg_type, data, timeout_ms)
```

### Timers

```lua
-- Skynet
skynet.timeout(100, function() ... end)

-- Shield
shield.timeout(100, function() ... end)
```

### Service Discovery

```lua
-- Skynet
local handle = skynet.uniqueservice("service_name")

-- Shield
local handle = shield.uniqueservice("service_name")
```

### Fork

```lua
-- Skynet
skynet.fork(function() ... end)

-- Shield (via fork on the C++ side, Lua gets shield.send for async work)
shield.send("worker_service", "task", data)
```

## What Shield Adds

1. **Cross-platform**: Runs on Windows out of the box
2. **Multi-protocol gateway**: HTTP, WebSocket, TCP, UDP in one process
3. **Middleware pipeline**: Cross-protocol auth, logging, CORS
4. **Type-safe C++ service API**: `service::send/call/query/timeout` as C++ free functions
5. **VM pooling**: Shared Lua VM pool for efficient memory usage
6. **Debug console**: TCP-based runtime inspection (`telnet localhost 13000`)
7. **Dependency management**: vcpkg for reproducible builds

## What Shield Does NOT Cover

- Skynet's `harbor` cluster router (use CAF distribution + discovery instead)
- Skynet's `snax` service framework (use Shield's `LuaServiceBase` instead)
- Skynet's `stm` (share-nothing; Shield follows the same model via actors)
