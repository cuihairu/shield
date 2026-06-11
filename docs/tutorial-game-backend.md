# Tutorial: Multiplayer Game Backend

This tutorial is a **target design sketch** for the refactored Shield runtime. It
is not a currently verified runnable guide.

## Target Structure

```text
my-game/
├── config/app.yaml
└── scripts/
    ├── gateway.lua
    ├── room.lua
    └── chat.lua
```

## Configuration

```yaml
app:
  name: my_game

log:
  level: info
  console: true

actors:
  - name: gateway
    script: scripts/gateway.lua
    network:
      tcp: "0.0.0.0:8001"

  - name: room
    script: scripts/room.lua
    instances: 1

  - name: chat
    script: scripts/chat.lua
    instances: 1
```

## Gateway Service

```lua
local M = {}
local sessions = {}

function M.on_connect(session)
    sessions[session:id()] = session
end

function M.on_client_message(session, payload)
    if payload.type == "join_room" then
        shield.send("room", "join", {
            session_id = session:id(),
            player_id = payload.player_id,
            room_id = payload.room_id
        })
    elseif payload.type == "chat" then
        shield.send("chat", "send", {
            session_id = session:id(),
            player_id = payload.player_id,
            room_id = payload.room_id,
            text = payload.text
        })
    end
end

function M.room_joined(data)
    local session = sessions[data.session_id]
    if session then
        session:send({ type = "room_joined", room_id = data.room_id })
    end
end

function M.chat_ok(data)
    local session = sessions[data.session_id]
    if session then
        session:send({ type = "chat_ok", room_id = data.room_id })
    end
end

function M.on_disconnect(session)
    sessions[session:id()] = nil
end

return M
```

## Room Service

```lua
local M = {}
local rooms = {}

function M.join(data)
    local src = shield.sender()
    local id = data.room_id
    rooms[id] = rooms[id] or { players = {} }
    table.insert(rooms[id].players, data.player_id)
    shield.send(src, "room_joined", {
        session_id = data.session_id,
        room_id = id,
    })
end

return M
```

## Chat Service

```lua
local M = {}

function M.send(data)
    local src = shield.sender()
    shield.log.info("chat " .. data.room_id .. ": " .. data.text)
    shield.send(src, "chat_ok", {
        session_id = data.session_id,
        room_id = data.room_id,
    })
end

return M
```

## What This Demonstrates

- Gateway is a Lua service pattern, not a middleware framework.
- Business services communicate through `shield.send` and `shield.call`.
- Configuration declares services and network binding.
- HTTP endpoints, Prometheus, service discovery, and multi-node deployment are
  outside this tutorial's core scope.

This page should become a runnable tutorial only after module-table Lua services,
`shield.spawn`, network callbacks, and the final startup entrypoint are
implemented and tested.
