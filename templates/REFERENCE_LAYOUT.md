# Shield Reference Layout: Gateway + Logic + Storage

Recommended project structure for a production game server built on Shield.

```
my-game-server/
├── config/
│   ├── app.yaml              # Main configuration
│   ├── app-dev.yaml           # Development overrides
│   └── app-prod.yaml          # Production overrides
├── scripts/
│   ├── init.lua               # Global initialization (loaded by all VMs)
│   ├── gateway/
│   │   ├── login.lua          # HTTP POST /login handler
│   │   ├── session.lua        # WebSocket session lifecycle
│   │   └── router.lua         # Message type routing table
│   ├── logic/
│   │   ├── player.lua         # Player state & actions
│   │   ├── room.lua           # Room/match management
│   │   ├── chat.lua           # Chat system
│   │   └── inventory.lua      # Inventory/economy
│   └── storage/
│       └── persistence.lua    # Database read/write adapter
├── build/                     # CMake build output
├── logs/                      # Runtime logs
└── README.md
```

## Configuration (config/app.yaml)

```yaml
app:
  name: "My Game Server"

log:
  global_level: "info"
  console:
    enabled: true

server:
  host: "0.0.0.0"
  port: 8080

lua:
  script_dir: "scripts/"
  auto_reload: true
  preload_scripts:
    - "init.lua"

gateway:
  listener:
    host: "0.0.0.0"
    port: 8080
  tcp:
    enabled: true
  http:
    enabled: true
    port: 8082
    backend: "beast"
  websocket:
    enabled: true
    port: 8083
    lua_script: "scripts/gateway/session.lua"
```

## Gateway Layer (scripts/gateway/)

Handles client connections. No game state.

**login.lua** — HTTP POST /login
- Validates credentials
- Creates session token
- Returns player_id + token

**session.lua** — WebSocket lifecycle
- Authenticates connection via token
- Binds connection to player actor
- Handles heartbeat / disconnect

**router.lua** — Message dispatch
- Reads `type` field from incoming messages
- Routes to the appropriate logic actor via `shield.send()` / `shield.call()`

## Logic Layer (scripts/logic/)

Pure game logic. No network code.

Each script is a Shield Lua actor with `on_init()` + `on_message()`:

```lua
-- player.lua
local player = {}

function on_init()
    player.level = 1
    player.exp = 0
end

function on_message(msg)
    if msg.type == "get_info" then
        return { success = true, data = { level = tostring(player.level) } }
    elseif msg.type == "gain_exp" then
        player.exp = player.exp + tonumber(msg.data.amount or "0")
        return { success = true }
    end
end
```

## Storage Layer (scripts/storage/)

Abstracts database access behind service calls.

```lua
-- persistence.lua
function on_message(msg)
    if msg.type == "save_player" then
        -- shield.call("database", "execute", ...)
        return { success = true }
    elseif msg.type == "load_player" then
        return { success = true, data = { level = "1" } }
    end
end
```

## Running

```bash
# Build
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build

# Run
./build/bin/shield server --config config/app.yaml

# Development (with auto-reload)
./build/bin/shield server --config config/app-dev.yaml

# Production
./build/bin/shield server --config config/app-prod.yaml
```

## Multi-Node Deployment

For multi-node, run separate Shield processes:

```bash
# Node 1: Gateway (handles clients)
shield server --config config/gateway.yaml

# Node 2: Logic (game state)
shield server --config config/logic.yaml

# Node 3: Storage (database access)
shield server --config config/storage.yaml
```

Nodes discover each other via the configured discovery mechanism
(static list, Redis, etcd, consul, or nacos).
