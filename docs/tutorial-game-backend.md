# Tutorial: Multiplayer Game Backend

This tutorial is a **target design sketch** for the Shield runtime. It is not a
currently verified runnable guide — do NOT copy from here expecting it to boot.

**Verified runnable paths** (use these instead):

| Goal | Entry |
| --- | --- |
| 10-minute linear onboarding (build → start → connection check) | [Quick Start](quickstart.md) |
| Generate your own minimal project with one command | `scripts/new_project.sh <dir>` |
| Full client RPC loop (auth/binding/forwarding, with real TCP e2e tests) | `examples/hello_world/` |

The value of this document is explaining "why it is designed this way" (single target,
bind switching, private forwarding);
the code snippets are illustrative conventions; for fields and APIs the Lua API Contract and
Runtime Config Semantics prevail.

## Target Structure

```text
my-game/
├── config/app.yaml
└── scripts/
    ├── auth.lua
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
    script: scripts/auth.lua
    network:
      tcp: "0.0.0.0:8001"
    rpc:
      routes:
        - { id: 1, name: login, direction: c2s, binding: login, requires_auth: false }

  - name: player
    script: scripts/player.lua
    instances: 1
    rpc:
      routes:
        - { id: 100, name: login_result, direction: s2c, binding: login_result }
        - { id: 2, name: join_room, direction: c2s, binding: join_room, requires_auth: true }
        - { id: 200, name: room_joined, direction: s2c, binding: room_joined }
        - { id: 3, name: chat, direction: c2s, binding: chat, requires_auth: true }
        - { id: 300, name: chat_ok, direction: s2c, binding: chat_ok }

  - name: room
    script: scripts/room.lua
    instances: 1

  - name: chat
    script: scripts/chat.lua
    instances: 1
```

## Gateway Service (the auth entry point)

The Gateway boundary binds each session to a single target: this service before login, and after a
successful login `shield.client.bind` switches atomically to player. The dynamic routing of
room/chat is managed by player's private state and does not pass through the Gateway.

```lua
local M = {}

-- ClientControlMessage::Bound: the client connects to this (auth entry) service
function M.on_client_bound(ctx, client)
    shield.log.info("client " .. client:session_id() .. " connected")
end

-- route 1: login; on success the single target switches atomically to player (epoch incremented)
function M.login(ctx, client, request)
    local ok, ref = shield.client.bind(client, request.player_id, "player")
    if not ok then
        return
    end
    shield.client_rpc.login_result(ref, { ok = true })
end

-- ClientControlMessage::Disconnected
function M.on_disconnect(ctx, client, reason)
    shield.log.info("client disconnected: " .. reason)
end

return M
```

## Player Service (the single target after login)

```lua
local M = {}
local refs = {}  -- player_id -> ClientRef (private state, not routed through the Gateway)

-- ClientControlMessage::Bound: the client switches to this service; record the s2c outbound reference
function M.on_client_bound(ctx, client)
    if client:player_id() ~= "" then
        refs[client:player_id()] = client:ref()
    end
end

-- route 2: join room (authentication is guaranteed by the Gateway's requires_auth check)
function M.join_room(ctx, client, request)
    shield.send("room", "join", {
        player_id = client:player_id(),
        room_id = request.room_id
    })
end

-- route 3: chat
function M.chat(ctx, client, request)
    shield.send("chat", "send", {
        player_id = client:player_id(),
        room_id = request.room_id,
        text = request.text
    })
end

-- internal replies from room/chat: find the ClientRef from private state, then send s2c outbound
function M.room_joined(ctx, data)
    local ref = refs[data.player_id]
    if ref then
        shield.client_rpc.room_joined(ref, { room_id = data.room_id })
    end
end

function M.chat_ok(ctx, data)
    local ref = refs[data.player_id]
    if ref then
        shield.client_rpc.chat_ok(ref, { room_id = data.room_id })
    end
end

-- ClientControlMessage::Disconnected
function M.on_disconnect(ctx, client, reason)
    refs[client:player_id()] = nil
end

return M
```

## Room Service

```lua
local M = {}
local rooms = {}

function M.join(ctx, data)
    local id = data.room_id
    rooms[id] = rooms[id] or { players = {} }
    table.insert(rooms[id].players, data.player_id)
    shield.send(ctx.sender, "room_joined", {
        session_id = data.session_id,
        room_id = id,
    })
end

return M
```

## Chat Service

```lua
local M = {}

function M.send(ctx, data)
    shield.log.info("chat " .. data.room_id .. ": " .. data.text)
    shield.send(ctx.sender, "chat_ok", {
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

The prerequisites this tutorial depends on — module-table Lua services,
`shield.spawn`, network callbacks, and the startup entrypoint — are all
implemented and tested (see the roadmap Phase 2-3 status);
the steps below walk through the templates and examples in this repository.
