# 网关设计

重构后，gateway 不是一个带中间件策略的独立框架层，而是推荐的 Lua 服务模式。

## 目标

- `net` 负责连接监听、session 管理和原始收发。
- `transport` 负责可选解帧、路由元数据提取和私有协议适配。
- Lua gateway 服务负责登录、会话绑定、消息路由和业务策略。
- 鉴权、限流、踢线、心跳等逻辑优先放在 Lua gateway 中。

## 目标回调

```lua
local M = {}

function M.on_init()
    shield.log.info("gateway started")
end

function M.on_connect(session)
end

function M.on_disconnect(session, reason)
end

function M.on_client_message(session, payload)
end

-- 仅在 actors[].network.protocol 启用时调用。
function M.on_client_packet(session, packet, payload)
end

return M
```

`on_client_message` 是 legacy frame path。`on_client_packet` 是 protocol path，`packet` 是普通 Lua table，包含 `route_id`、`kind`、`flags`、`seq`、`action`、`target_service`、`codec_id`、`schema_id`、`route` 等路由元数据；`payload` 是 envelope 切出的 body 字节串。

## 消息流

```
client socket
  → net
  → optional transport protocol
  → Lua gateway service (`on_client_message` 或 `on_client_packet`)
  → shield.send / shield.call
  → Lua business service
```

## 非目标

以下能力不再作为 core gateway 设计：

- 跨协议 HTTP middleware chain。
- 内置 CORS / auth middleware。
- 内置 `/health`、`/status`、`/metrics` 管理端点。
- 框架级路由 DSL。

如果项目需要 HTTP 管理接口或复杂认证链，应在业务层或独立扩展中实现。
