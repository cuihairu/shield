# 客户端 RPC 路由设计

本文冻结 Shield 客户端 RPC 的路由契约。wire header 携带 `route_id`；body 是纯业务数据。

## 核心模型

```text
Client → Gateway → session.target Service → route_id → handler
```

- 所有客户端消息经过 Gateway 后，发给 **session 绑定的目标服务**。
- 登录前：session.target = AuthService。
- 登录后：session.target = PlayerService。
- **route_id 的唯一职责**：在目标 Service VM 内选择 handler。
- Gateway 不从 route_id 解析目标服务，不做多目标路由，不知道 room/scene/map。
- 目标服务转发给其他服务由 Lua 内部决定，不在 Gateway 层面参与。

## Wire 格式

```text
+------------------+------------------+
|     Header       |       Body       |
| (route_id, ...)  | (业务数据)        |
+------------------+------------------+
```

- Header 携带 `route_id`、帧长度、校验等传输层字段。
- Body 是纯业务数据，编码格式由 ProtocolProfile 决定（json / protobuf / msgpack）。
- route_id 不出现在 body 中。body 不携带路由、envelope 或包装。

## RPC 描述符

每个客户端 RPC 在编译期由描述符定义：

```text
RpcMethodDescriptor {
  route_id              uint32    wire header 中的方法标识，ProtocolProfile 内唯一
  full_name             string    完整方法名，如 "player.move"，用于日志和调试
  direction             enum      client_to_server | server_to_client
  request_schema        schema    请求体结构（仅 client_to_server）
  response_schema?      schema    响应体结构（仅 server_to_client）
  binding_hint          string    目标 Lua handler 函数名，如 "move"
}
```

约束：

- `route_id` 在一个 ProtocolProfile 内唯一，写入 wire header。
- `full_name` 只用于契约、代码生成、日志和调试，不进入 wire。
- direction 不匹配的消息在 Gateway 拒绝。
- 描述符在编译期确定，目标 Service 启动时编译为 `route_id → cached Lua handler` 映射。

## 入站路径

```text
socket bytes
  → frame decode
  → 读 header.route_id
  → Gateway 路由表校验（route_id 合法 + direction + 认证要求）
  → session.target（AuthService 或 PlayerService）
  → CAF send ClientIngress { gateway_address, session_id, session_epoch, player_id,
                              protocol_profile_id, route_id, body_bytes }
  → 目标 VM 收到 ClientIngress
  → binding cache: route_id → cached handler
  → 按 request_schema decode body_bytes → 业务参数
  → invoke handler(client_context, 业务参数)
```

关键点：

- Gateway 只读 header 中的 route_id 做合法性校验，不解码 body。
- `body_bytes` 原样传递到目标 VM，由目标 VM 按 schema 解码。
- handler 签名固定为 `handler(ClientContext, decoded_request)`，不含 route_id 或原始 frame。

## 预登录路由

认证前 session 尚未绑定 PlayerService。预登录 RPC（login、token 验证等）走以下路径：

```text
Client → Gateway → session.target = AuthService
  → AuthService handler 处理登录逻辑
  → 认证成功
  → Gateway 原子切换 session.target = PlayerService
```

- 预登录 RPC 使用同样的 route_id → handler 机制，目标是 AuthService。
- 认证成功后 Gateway 原子更新 session 的 target、player_id 和 epoch。
- 认证前访问需认证的 route_id，Gateway 直接拒绝。

## 出站路径

```text
Lua 业务代码调用 codegen helper:
  player_rpc.move_result(client_context, { accepted = true })

helper 内部:
  → 按 response_schema encode 业务参数为 body_bytes
  → CAF send ClientEgress { session_id, session_epoch, route_id, body_bytes } 到 gateway_address

Gateway 收到 ClientEgress:
  → 校验 session_id + session_epoch
  → 把 route_id 写入 wire header
  → body_bytes 作为 body
  → frame encode → socket write
```

- `player_rpc.move_result` 是 codegen 生成的 server-to-client helper，已绑定 `route_id` 和 `response_schema`。
- 业务代码只传 `ClientContext` 或 `ClientRef` + 业务参数。
- Lua 不提供按字符串 route 或裸 `route_id` 的通用发送接口。

## Gateway 路由表

Gateway 维护一张轻量校验表，编译期从描述符集构建：

```text
GatewayRouteTable:
  route_id → { direction, requires_auth }
```

- 只做合法性校验：route_id 是否存在、方向是否允许客户端发起、是否需要认证。
- 不含 logical_service_name、handler、schema 或 ServiceAddress。
- 校验失败的消息在 Gateway 直接拒绝，不进入目标 VM。

## Session 绑定

```text
Session {
  target           ServiceHandle    当前目标服务（AuthService 或 PlayerService）
  player_id        string?          可信身份，认证后由 runtime 注入
  session_epoch    uint32           每次绑定更新递增
  protocol_profile string           编解码配置标识
}
```

- 登录前：target = AuthService，player_id = nil。
- 登录后：target = PlayerService，player_id = 认证结果。
- 进入房间/场景/地图：session binding 不变（仍指向 PlayerService）。动态路由由 PlayerService 私有状态管理。
- 旧 epoch 的消息必须拒绝或丢弃。

## ClientIngress 消息

客户端 RPC 进入目标 actor 时使用的内部消息：

```text
ClientIngress {
  gateway_address       ServiceAddress
  session_id            uint32
  session_epoch         uint32
  player_id             string?
  protocol_profile_id   string
  route_id              uint32          来自 wire header
  body_bytes            bytes           纯业务数据，原样传递
}
```

这是 CAF/Shield runtime 内部消息，不是客户端 wire 格式，也不是 Lua API。CAF behavior 按内部消息类型区分 `ClientIngress`、普通 service send/call、生命周期控制等类别。

## ClientEgress 消息

服务端发送 response/push 到 Gateway 时使用的内部消息：

```text
ClientEgress {
  session_id            uint32
  session_epoch         uint32
  route_id              uint32          由 codegen helper 绑定
  body_bytes            bytes           按 response_schema 编码的业务数据
}
```

Gateway 收到后校验 session，把 route_id 写入 header，body_bytes 作为 body，发送到客户端。

## ClientContext

每个客户端 RPC handler 的第一个参数是只读 `ClientContext`：

```lua
client:player_id()    -- 可信身份；预登录 RPC 为 nil
client:ref()          -- 可序列化 ClientRef（Gateway 地址、session id、epoch、player_id）
```

规则：

- `player_id` 来自 Gateway 认证绑定，不信任客户端 body 中的同名字段。
- `ClientRef` 可保存到服务状态或作为 service 消息参数传递，不是 actor reference。
- `ClientContext` 不暴露 route_id、route name、原始 frame、codec 或 CAF handle。

## 错误与安全

以下情况在调用业务 handler 前拒绝：

- 未知 route_id。
- direction 不允许客户端发起。
- 未认证 session 访问需认证的 route。
- session epoch 已过期。
- body 不符合 request_schema（在目标 VM decode 阶段）。
- 目标 VM 缺少 handler binding（启动期就应失败，不进入运行时）。

## 明确不做

- Gateway 多目标路由（session.service_routes[room/scene/map]）。
- Gateway 知道 logical_service_name。
- bit-segment route_id 编码服务类型。
- Lua 二次 if/else 路由。
- 业务暴露 route_id、原始 frame 或 codec。
- 每个 RPC 映射一个静态 CAF C++ 消息类型。
