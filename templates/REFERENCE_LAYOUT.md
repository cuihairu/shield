# Shield Reference Layout

This layout reflects the refactor target: a single-node Lua-first game server
runtime.

```text
my-game-server/
├── config/
│   └── app.yaml
├── scripts/
│   ├── gateway.lua
│   ├── player.lua
│   ├── room.lua
│   └── chat.lua
├── logs/
└── README.md
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

  - name: player
    script: scripts/player.lua
    instances: 0
```

## Gateway

`gateway.lua` handles client sessions and routes messages to logic services.

```lua
local gateway = shield.service("gateway")

function gateway.on_session_message(session_id, msg_type, data)
    if msg_type == "join_room" then
        shield.send("room", "join", {
            session = session_id,
            player_id = data.player_id
        })
    end
end
```

## Logic Services

```lua
local room = shield.service("room")

function room.on_message(src, msg_type, data)
    if msg_type == "join" then
        shield.send(src, "joined", { ok = true })
    end
end
```

## Out Of Core

The following old reference-layout ideas are no longer core defaults:

- Multi-node gateway / logic / storage deployment.
- Built-in service discovery through Redis / Etcd / Consul / Nacos.
- Prometheus and health-check endpoints.
- Middleware chains.

Users can still build these patterns externally, but they are not part of the
current Shield core contract.
