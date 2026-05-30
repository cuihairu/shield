# Quickstart

Get a Shield game server running in 5 minutes.

## Prerequisites

- C++20 compiler (MSVC / GCC 11+ / Clang 14+)
- CMake 3.30+
- [vcpkg](https://vcpkg.io/) (for dependency management)
- Lua 5.4+ (installed via vcpkg)

## 1. Clone and Build

```bash
git clone https://github.com/cuihairu/shield.git
cd shield

# Set vcpkg root
export VCPKG_ROOT=/path/to/vcpkg

# One-command build
./build.sh release
```

On Windows:

```cmd
set VCPKG_ROOT=C:\path\to\vcpkg
build.bat release
```

## 2. Run the Server

```bash
./build/bin/shield server --config config/app.yaml
```

You should see:

```
[INFO] Shield Server starting...
[INFO] Script Starter initialized successfully
[INFO] Actor Starter initialized successfully
[INFO] Service Starter initialized successfully
[INFO] Gateway Starter initialized successfully
[INFO] HTTP server started on port 8082
[INFO] WebSocket server started on port 8083
[INFO] Application running. Press Ctrl+C to exit.
```

## 3. Test It

```bash
# Health check
curl http://localhost:8082/health

# Runtime status
curl http://localhost:8082/status

# Send a game action
curl -X POST http://localhost:8082/api/game/action \
  -H "Content-Type: application/json" \
  -d '{"action":"ping"}'
```

## 4. Write a Lua Service

Create `scripts/my_service.lua`:

```lua
local state = { count = 0 }

function on_init()
    log_info("My service initialized")
end

function on_message(msg)
    if msg.type == "ping" then
        state.count = state.count + 1
        return {
            success = true,
            data = {
                message = "pong",
                count = tostring(state.count)
            }
        }
    end

    -- Use Skynet-style API to call other services
    if msg.type == "get_player" then
        local result = shield.call("player_manager", "get_info", {
            player_id = msg.data.player_id or ""
        })
        return result
    end

    return { success = false, error_message = "Unknown: " .. msg.type }
end
```

## 5. Connect via WebSocket

```javascript
const ws = new WebSocket("ws://localhost:8083/ws");
ws.onopen = () => ws.send(JSON.stringify({ type: "ping" }));
ws.onmessage = (e) => console.log(JSON.parse(e.data));
```

## 6. Debug Console

```bash
telnet localhost 13000
# Type: list
# Type: info player_manager
# Type: call debug_console {"type":"ping"}
```

## Next Steps

- [Template reference](templates/REFERENCE_LAYOUT.md) for project structure
- [Skynet comparison](skynet-comparison.md) for migration from Skynet
- [CAF mapping](caf-mapping.md) for understanding the actor layer
- [API reference](api/) for complete API docs
