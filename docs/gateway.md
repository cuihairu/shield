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
  -> Lua gateway on_client_message(session, client)
     -- client = { route = "c2s.player.move", payload = {...} }
```

`client.route` 是规范逻辑路由名（来自 `RouteTable` 中注册的 route），`client.payload` 是 codec 解码后的业务值。`route_id`、`codec_id`、`schema_id`、`PacketKind`、`flags`、`seq`、原始帧等 wire 细节不进入 Lua。

### 出站

```text
Lua gateway / business service
  -> session:send({ route = "s2c.player.move_result", payload = {...} })
  -> RouteResolver（仅从外层 route 名解析，不扫描 payload 内字段）
  -> BodyCodec encode
  -> Envelope encode
  -> shield_net write
```

出站只从外层 `route` 字段解析路由，不再从 payload 内 `route_id/msg_id/route/method` 猜 route。业务 payload 自然拥有名为 `route/method/id` 的字段时，不会影响 transport 路由。

这个方向的核心是把进站和出站流向固定下来，类似 pipeline 模式。Lua 只处理业务消息，不承担 transport 数据面的分支判断。客户端消息形状与具体 codec 无关，见 [架构决策记录](architecture-decisions.md) AD-05。

## 协议部件

在这条固定骨架里，协议实现只需要填充固定槽位：

- `Envelope`：解帧 / 封帧
- `RouteExtractor`：进站 route 提取
- `RoutePolicy`：`DecodeLocal` / `ForwardRaw` / `Drop`
- `BodyCodec`：业务消息 decode / encode
- `RouteResolver`：出站 route 解析

因此 `json`、`msgpack`、`protobuf`、`sproto`、`xmldef` 这类协议，不应该被建模成“Lua 前面的一串任意节点”，而应该被建模成固定骨架里的协议部件组合。

更进一步说，gateway 真正绑定的应是一条 session 级 `ProtocolProfile`，而不是“每包自报家门”。系统内部可以同时存在多种 profile，但同一条客户端连接一旦建立，就应固定使用其中一套。

## Lua 边界

- Lua 侧应只接收 `DecodeLocal` 后的逻辑消息 `client = { route, payload }`。
- `body.codec: raw` 时，`client.payload` 是字节串，但它仍是显式 decode 结果，不是未解码的 transport payload。
- `ForwardRaw`、`Drop`、协议解析错误应在 C++ 数据面终止，不应再透传给 Lua。
- Lua gateway 关注的是业务语义，而不是 frame/header/body 的中间态。
- 业务 Lua 不应接收 `route_id`、`PacketKind`、`seq`、raw frame 或 envelope/header。

## 跨服协作

gateway 进入业务层之后，默认跨服协作应立即切回逻辑消息模型：

```text
gateway decoded client message
  -> shield.send / shield.call
  -> target local/remote service（如 PlayerService）
```

规则：

- 默认跨服走 `shield.send/call`，而不是继续转发客户端协议帧。
- gateway 对客户端连接负责，业务 service 对业务状态负责。
- 如果目标 service 在远端节点，后续由 `shield_cluster` 扩展为远端路由；gateway 自身不应该感知 CAF 远程细节。
- `ForwardRaw` 只保留给少数代理/协议透传场景，不作为常规业务协作方式。

### Gateway ↔ PlayerService 的固定 service method

Gateway 持有 `SessionHandle`，负责 session↔uid 绑定和最终编码写回；PlayerService 不持有 `SessionHandle`，只持 `session_id` 标量。两者之间用普通 service 消息协作（见 [架构决策记录](architecture-decisions.md) AD-02/AD-06）：

```lua
-- Gateway -> PlayerService：投递客户端消息
shield.send(player_service, "client_message", {
    session_id = session:id(),
    route = client.route,
    payload = client.payload,
})

-- PlayerService -> Gateway：回送 / push
shield.send(gateway_service, "client_send", {
    session_id = state.session_id,
    route = "s2c.player.move_result",
    payload = { accepted = true },
})

-- Gateway -> PlayerService：断线通知
shield.send(player_service, "client_disconnect", {
    session_id = session:id(),
    reason = reason,
})
```

Gateway 的 `client_send` 负责按 `session_id` 查 live session、校验当前 owner（防断线/重连/stale 误投），再 `session:send({route, payload})`。

### 客户端 request / response / push 边界

- 客户端 request = 一个 `client_to_server` route 的业务消息，Gateway `shield.send` 投递，不阻塞、不等返回。
- 客户端 response / push = 一个 `server_to_client` route，走 `client_send` → `session:send`，二者同路径，仅 route 语义区分。
- correlation token（request id 等）是业务 payload schema 字段，框架不维护 pending 表，不提供框架级 client RPC（见 AD-05）。`PacketKind` / `seq` 不暴露为 Lua client API。

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

-- client 是 DecodeLocal 后的逻辑消息。
function M.on_client_message(session, client)
    -- client.route    : 规范逻辑路由名（如 "c2s.player.move"）
    -- client.payload  : 解码后业务值；json/msgpack/protobuf 为 table，raw 为字节串
end

-- PlayerService 回送 / push 的固定 service method。
function M.client_send(command)
    -- command.session_id / command.route / command.payload
end

return M
```

## 当前 Phase 1 快照

- 未配置 `actors[].network.protocol` 时，仍走 legacy frame path，并调用 `on_client_message(session, payload)`（payload 为字节串）。
- 已配置 `network.protocol` 时，真实 gateway Lua 路径调用 `on_client_message(session, client)`，其中 `client = { route, payload }`。
- 当前实际是 listener-fixed profile：listener 配什么，接入的 session 就绑定什么。
- `DecodeLocal` 才会进入 Lua；`ForwardRaw`、`Drop` 和协议错误停留在 C++ 数据面。
- `body.codec = json` / `msgpack` 时，`client.payload` 是 table；`body.codec = raw` 时是字节串。
- `protobuf` 需要通过 `body.provider` 引用 `shield.protocol.codec.v1` 插件后才能作为 `DecodeLocal` 进入 Lua；未配置 provider 时仍只是占位 codec。
- `sproto`、`xmldef`、`fbs` 这类尚未落地真实 provider 的 codec 当前不能作为 `DecodeLocal` 进入 Lua。

> **契约收敛提示（路由闭环，见 [架构决策记录](architecture-decisions.md) AD-05）**：
>
> 目标设计是 **route_id 一次查表直达 handler**：注册表在启动时建立 `route 名 ↔ route_id ↔ handler ↔ target service` 双向映射（route 名保留不丢弃，便于调试；route 本身是 int 时直接用，是字符串才 hash）；运行时 C++ 解出 route_id 一次查表即得「目标进程」与「handler 函数」，业务 handler 不写 if/else。
>
> 当前实现是**未闭环的半成品**：`RouteTable` 只建了 `route_id ↔ route_name` 前半截（`include/shield/transport/protocol.hpp:91-107`），**没有** `route_id ↔ handler`、`route_id ↔ target service` 的后半截；`LuaGatewayBridge` 不读 `target_service`、全投给同一 gateway（`src/lua/lua_gateway_bridge.cpp:26-27`），且进入 Lua 时丢弃 route、只传 payload（`src/lua/lua_gateway_bridge.cpp:72-88`）。结果是业务被迫在 `on_client_message` 内自己 if/else 二次判断——即「路由两次」。
>
> 另：codec 与路由无关，是 session 级固定（`codec_for_route` 忽略 route，恒返回 `default_codec_id`，`src/transport/protocol.cpp:1510-1517`）。解析 route_id 的唯一目的是识别消息种类、定位 handler，不是选 codec。
>
> 补闭环为 roadmap 项（见 [客户端交互契约收敛](roadmap.md)）。

正常业务包头不应重复携带“我是 json/protobuf/sproto”这类协议家族标识；这些信息应在 listener 配置或首阶段握手时已经确定。

对于 `xmldef`，后续目标不是只补一个服务端 decoder，而是补齐 `xml -> descriptor -> generator plugin -> runtime binding` 全链路；见 [Xmldef Toolchain Design](xmldef-toolchain-design.md) 和 [Xmldef Compiler / Runtime MVP](xmldef-compiler-runtime-mvp.md)。

## 非目标

以下能力不再作为 core gateway 设计：

- 跨协议 HTTP middleware chain。
- 内置 CORS / auth middleware。
- 内置 `/health`、`/status`、`/metrics` 管理端点。
- 框架级路由 DSL。

如果项目需要 HTTP 管理接口或复杂认证链，应在业务层或独立扩展中实现。
