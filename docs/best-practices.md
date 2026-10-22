# Best Practices

The APIs and semantics in this document are all implemented; for concrete return values and scheduling semantics see the Lua API Contract and [Runtime Semantics](runtime-semantics.md).

## Service Decomposition

One Lua service handles one category of business; do not create one service per API.

```lua
local M = {}

function M.join(player)
    -- handle join
end

function M.leave(player)
    -- handle leave
end

return M
```

## Async First

Prefer `shield.send`. Use `shield.call` only when a result is needed.

```lua
shield.send("room", "player_joined", { player_id = "1001" })
local ok, result = shield.call_timeout(3000, "player", "get_info", { id = "1001" })
```

## Gateway Logic

The authentication entry point only does authentication and binding; business dispatch stays inside the target service — do not re-route by body in the Gateway.

```lua
-- auth service: login is a c2s descriptor binding
function M.login(ctx, client, request)
    local player_id = authenticate(request)
    if not player_id then
        return {code = "auth_failed"}
    end
    -- suspend the coroutine; CAS switches the single target to "player"; on resume the ClientRef carries the new epoch
    local ok, ref = shield.client.bind(client, player_id, "player")
    if not ok then
        return {code = ref.code}
    end
    shield.client_rpc.login_result(ref, {player_id = ref:player_id()})
    return {code = "ok"}
end
```

Outbound traffic always goes through the `shield.client_rpc.<name>` helpers registered per s2c descriptor; never write a bare
route_id, and never do a generic send.

## Data Access

Use plugin namespaces and binding logical names to access backend capabilities; do not tie business models to an ORM.

```lua
local db = shield.database.mysql("database.default")
local ok, rows = db:query("SELECT * FROM users WHERE id = ?", { id })

local q = shield.queue.redis("queue.events")
q:publish("chat.world", { from = id, text = "hello" })
```

## Configuration

- YAML is for binding only.
- Do not put business logic into configuration.
- Prefer injecting secrets through the environment or the deployment system.
- Do not depend on legacy core fields such as discovery / metrics / plugin.

## Logging

Record business logs through `shield.log.*`:

```lua
shield.log.info("player login: " .. player_id)
shield.log.warn("heartbeat timeout: " .. player_id)
shield.log.error("load profile failed: " .. err)
```

Prometheus, health checks, and centralized monitoring are not part of the current core best practices; when needed, use external systems or later standalone extensions.
