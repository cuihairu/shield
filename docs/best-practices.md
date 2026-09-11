# 最佳实践

本文基于重构后的目标设计，仍属于设计阶段建议。

具体 API 返回值和调度语义见 [运行时语义决策稿](./runtime-semantics.md)。

## 服务拆分

一个 Lua 服务处理一类业务，不要一个 API 一个服务。

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

## 异步优先

优先使用 `shield.send`。只有需要结果时才使用 `shield.call`。

```lua
shield.send("room", "player_joined", { player_id = "1001" })
local ok, result = shield.call_timeout(3000, "player", "get_info", { id = "1001" })
```

## 网关逻辑

认证入口只做认证与绑定;业务分发留在 target 服务内,不在 Gateway 按 body 二次路由。

```lua
-- auth 服务:login 是 c2s descriptor binding
function M.login(client, request)
    local player_id = authenticate(request)
    if not player_id then
        return {code = "auth_failed"}
    end
    -- 挂起协程,CAS 切换单一 target 到 "player";恢复时拿到新 epoch 的 ClientRef
    local ok, ref = shield.client.bind(client, player_id, "player")
    if not ok then
        return {code = ref.code}
    end
    shield.client_rpc.login_result(ref, {player_id = ref:player_id()})
    return {code = "ok"}
end
```

出站一律走按 s2c descriptor 注册的 `shield.client_rpc.<name>` helper,不写裸
route_id,不做通用 send。

## 数据访问

使用插件 namespace 和 binding 逻辑名访问后端能力，不把业务模型绑定到 ORM。

```lua
local db = shield.database.mysql("database.default")
local ok, rows = db:query("SELECT * FROM users WHERE id = ?", { id })

local q = shield.queue.redis("queue.events")
q:publish("chat.world", { from = id, text = "hello" })
```

## 配置

- YAML 只做绑定。
- 不把业务逻辑写进配置。
- 敏感信息优先通过环境或部署系统注入。
- 不依赖 discovery / metrics / plugin 等旧 core 字段。

## 日志

通过 `shield.log.*` 记录业务日志：

```lua
shield.log.info("player login: " .. player_id)
shield.log.warn("heartbeat timeout: " .. player_id)
shield.log.error("load profile failed: " .. err)
```

Prometheus、健康检查、集中监控不属于当前 core 最佳实践；需要时使用外部系统或后续独立扩展。
