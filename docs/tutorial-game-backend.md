# Tutorial: Multiplayer Game Backend

Build a simple multiplayer game backend with Shield: login, room management, and real-time chat.

## Project Structure

```
my-game/
├── config/app.yaml
└── scripts/
    ├── init.lua
    ├── login.lua
    ├── room.lua
    └── chat.lua
```

## Step 1: Configuration

```yaml
# config/app.yaml
app:
  name: "My Game"

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
  http:
    enabled: true
    port: 8082
    backend: "beast"
  websocket:
    enabled: true
    port: 8083
    path: "/ws"
```

## Step 2: Init Script

```lua
-- scripts/init.lua
game = {
    players = {},
    rooms = {},
    next_room_id = 1
}

function ok(data)
    return { success = true, data = data or {} }
end

function fail(msg)
    return { success = false, error_message = msg or "error" }
end

log_info("Game initialized")
```

## Step 3: Login Service

```lua
-- scripts/login.lua
local tokens = {}

function on_init()
    log_info("Login service ready")
end

function on_message(msg)
    if msg.type == "login" then
        local username = msg.data.username or ""
        if username == "" then return fail("Missing username") end

        local token = username .. "_" .. tostring(get_current_time())
        local player_id = "player_" .. username

        tokens[token] = {
            player_id = player_id,
            username = username
        }
        game.players[player_id] = { username = username, online = true }

        log_info("Player logged in: " .. username)
        return ok({ token = token, player_id = player_id })
    end

    if msg.type == "verify" then
        local token = msg.data.token or ""
        local session = tokens[token]
        if not session then return fail("Invalid token") end
        return ok({ player_id = session.player_id })
    end

    return fail("Unknown type: " .. msg.type)
end
```

## Step 4: Room Service

```lua
-- scripts/room.lua
function on_init()
    log_info("Room service ready")
end

function on_message(msg)
    if msg.type == "create" then
        local room_id = "room_" .. tostring(game.next_room_id)
        game.next_room_id = game.next_room_id + 1

        game.rooms[room_id] = {
            id = room_id,
            players = {},
            max_players = tonumber(msg.data.max or "4")
        }

        log_info("Room created: " .. room_id)
        return ok({ room_id = room_id })
    end

    if msg.type == "join" then
        local room_id = msg.data.room_id or ""
        local player_id = msg.data.player_id or ""
        local room = game.rooms[room_id]

        if not room then return fail("Room not found") end
        if #room.players >= room.max_players then return fail("Room full") end

        table.insert(room.players, player_id)
        log_info(player_id .. " joined " .. room_id)

        -- Notify other players
        for _, pid in ipairs(room.players) do
            if pid ~= player_id then
                shield.send(pid, "room_event", {
                    event = "player_joined",
                    room_id = room_id,
                    player_id = player_id
                })
            end
        end

        return ok({ room_id = room_id, players = room.players })
    end

    if msg.type == "list" then
        local list = {}
        for id, room in pairs(game.rooms) do
            table.insert(list, {
                id = id,
                count = tostring(#room.players),
                max = tostring(room.max_players)
            })
        end
        return ok({ rooms = list })
    end

    return fail("Unknown type: " .. msg.type)
end
```

## Step 5: Chat Service

```lua
-- scripts/chat.lua
function on_init()
    log_info("Chat service ready")
end

function on_message(msg)
    if msg.type == "send" then
        local room_id = msg.data.room_id or ""
        local player_id = msg.data.player_id or ""
        local text = msg.data.text or ""

        if text == "" then return fail("Empty message") end

        local room = game.rooms[room_id]
        if not room then return fail("Room not found") end

        -- Broadcast to all players in the room
        for _, pid in ipairs(room.players) do
            shield.send(pid, "chat", {
                from = player_id,
                text = text,
                room_id = room_id,
                ts = tostring(get_current_time())
            })
        end

        return ok({ delivered = "true" })
    end

    return fail("Unknown type: " .. msg.type)
end
```

## Step 6: Run

```bash
./build/bin/shield server --config config/app.yaml
```

## Step 7: Test

```bash
# Login
curl -X POST http://localhost:8082/api/game/action \
  -d '{"type":"login","data":{"username":"alice"}}'

# WebSocket interaction
# Connect to ws://localhost:8083/ws and send:
# {"type":"create","data":{"max":"4"}}
# {"type":"join","data":{"room_id":"room_1","player_id":"player_alice"}}
# {"type":"send","data":{"room_id":"room_1","player_id":"player_alice","text":"Hello!"}}
```

## What You Learned

- How to structure a game backend with separate Lua services
- Using `shield.send()` for async notifications (chat broadcast)
- Using `shield.call()` for sync requests (not shown, same pattern)
- Room management with state tracking
- Token-based login flow
