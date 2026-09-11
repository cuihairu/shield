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

## Gateway Service（auth 入口）

Gateway 边界把每个 session 绑定到唯一 target：登录前是本服务，登录成功后
`shield.client.bind` 原子切换到 player。room/chat 的动态路由由 player 的
私有状态管理，不经过 Gateway。

```lua
local M = {}

-- ClientControlMessage::Bound：客户端接入本（auth 入口）服务
function M.on_client_bound(ctx, client)
    shield.log.info("client " .. client:session_id() .. " connected")
end

-- route 1：登录；成功后单一 target 原子切换到 player（epoch 递增）
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

## Player Service（登录后的单一 target）

```lua
local M = {}
local refs = {}  -- player_id -> ClientRef（私有状态，不经过 Gateway）

-- ClientControlMessage::Bound：客户端切换到本服务，记录 s2c 出站引用
function M.on_client_bound(ctx, client)
    if client:player_id() ~= "" then
        refs[client:player_id()] = client:ref()
    end
end

-- route 2：进房（认证由 Gateway 的 requires_auth 校验保证）
function M.join_room(ctx, client, request)
    shield.send("room", "join", {
        player_id = client:player_id(),
        room_id = request.room_id
    })
end

-- route 3：聊天
function M.chat(ctx, client, request)
    shield.send("chat", "send", {
        player_id = client:player_id(),
        room_id = request.room_id,
        text = request.text
    })
end

-- room/chat 的内部回包：按私有状态找回 ClientRef 后 s2c 出站
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

This page should become a runnable tutorial only after module-table Lua services,
`shield.spawn`, network callbacks, and the final startup entrypoint are
implemented and tested.
