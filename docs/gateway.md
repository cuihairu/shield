# 网关设计

重构后，gateway 不是一个带中间件策略的独立框架层，而是挂在固定网络 pipeline 末端的 Lua 服务模式。
协议差异通过固定槽位组件表达，而不是让 gateway 自己拼 middleware chain。

## 目标

- `net` 负责连接监听、session 管理和原始收发。
- `transport` 负责解帧、路由元数据提取、本地编解码和转发策略执行。
- Lua gateway 服务负责登录、会话绑定、消息路由和业务策略。
- 未解码的 transport payload 不应进入 Lua 边界。
- 鉴权、限流、踢线、心跳等逻辑优先放在 Lua gateway 中。

## 固定流向

### 入站

```text
client socket
  -> shield_net read
  -> Envelope decode frame
  -> RouteExtractor
  -> RouteTable / RoutePolicy
     -> Drop / Reject: C++ 结束
     -> ForwardRaw: C++ 转发路径结束
     -> DecodeLocal: BodyCodec decode
  -> Lua gateway on_client_message(session, message)
```

### 出站

```text
Lua gateway / business service
  -> produce business message
  -> RouteResolver
  -> BodyCodec encode
  -> Envelope encode
  -> shield_net write
```

这个方向的核心是把进站和出站流向固定下来，类似 pipeline 模式。Lua 只处理业务消息，不承担 transport 数据面的分支判断。

## 协议部件

在这条固定骨架里，协议实现只需要填充固定槽位：

- `Envelope`：解帧 / 封帧
- `RouteExtractor`：进站 route 提取
- `RoutePolicy`：`DecodeLocal` / `ForwardRaw` / `Drop`
- `BodyCodec`：业务消息 decode / encode
- `RouteResolver`：出站 route 解析

因此 `json`、`msgpack`、`protobuf`、`sproto`、`xmldef` 这类协议，不应该被建模成“Lua 前面的一串任意节点”，而应该被建模成固定骨架里的协议部件组合。

## Lua 边界

- Lua 侧应只接收 `DecodeLocal` 后的业务消息。
- `body.codec: raw` 是唯一允许“字节串进入 Lua”的场景；此时字节串是显式 decode 结果，不是未解码的 transport payload。
- `ForwardRaw`、`Drop`、协议解析错误应在 C++ 数据面终止，不应再透传给 Lua。
- Lua gateway 关注的是业务语义，而不是 frame/header/body 的中间态。

## 跨服协作

gateway 进入业务层之后，默认跨服协作应立即切回逻辑消息模型：

```text
gateway decoded message
  -> shield.send / shield.call
  -> target local/remote service
```

规则：

- 默认跨服走 `shield.send/call`，而不是继续转发客户端协议帧。
- gateway 对客户端连接负责，业务 service 对业务状态负责。
- 如果目标 service 在远端节点，后续由 `shield_cluster` 扩展为远端路由；gateway 自身不应该感知 CAF 远程细节。
- `ForwardRaw` 只保留给少数代理/协议透传场景，不作为常规业务协作方式。

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

-- message 是 DecodeLocal 后的业务消息。
-- 若 body.codec = raw，则 message 为字节串，但它仍是显式 decode 结果。
function M.on_client_message(session, message)
end

return M
```

## 当前 Phase 1 快照

- 未配置 `actors[].network.protocol` 时，仍走 legacy frame path，并调用 `on_client_message(session, payload)`。
- 已配置 `network.protocol` 时，真实 gateway Lua 路径也统一调用 `on_client_message(session, message)`。
- `DecodeLocal` 才会进入 Lua；`ForwardRaw`、`Drop` 和协议错误停留在 C++ 数据面。
- `body.codec = json` 时，Lua 收到的是 table；`body.codec = raw` 时，Lua 收到的是字节串。
- `msgpack`、`protobuf`、`sproto`、`xmldef`、`fbs` 这类尚未实现真实解码器的 codec 当前不能作为 `DecodeLocal` 进入 Lua。

## 非目标

以下能力不再作为 core gateway 设计：

- 跨协议 HTTP middleware chain。
- 内置 CORS / auth middleware。
- 内置 `/health`、`/status`、`/metrics` 管理端点。
- 框架级路由 DSL。

如果项目需要 HTTP 管理接口或复杂认证链，应在业务层或独立扩展中实现。
