# 网关设计

Gateway 是客户端连接、session 管理和 wire 入口的边界，不是业务 Lua 的通用消息分发器。

## 职责

- 每个 TCP listener 一个 Gateway actor,持有 live session 弱句柄注册表
  (`GatewaySessionRegistry`),管理连接、断线、踢下线和写回。
- 维护 session 绑定单一真相源(Session 对象):target(AuthService 或 PlayerService)、
  player_id、epoch、protocol profile;注册表只存连接期快照用于诊断。
- 只解析 frame/header 所需字段；普通客户端 RPC 只读取 header `route_id`。当监听器配置了 codec 插件（`network.protocol.body.provider`）时，pipeline 会把 body 解码为规范 JSON message；Gateway 本身不做业务解码，只负责把原始字节和解码结果一起转发。
- 用轻量路由表校验 route_id 合法性（存在、方向、认证要求），拒绝非法消息。
- 校验通过后把 `ClientIngress` 投递到 session.target 的 CAF actor。
- 接收目标 actor 的 `ClientEgress`，校验 session_id/epoch 后写入 wire header 并写 socket。

Gateway 不持有业务 handler，不做多目标路由，不知道 room/scene/map。

## Session 绑定

```text
Session {
  target           ServiceHandle    AuthService 或 PlayerService
  player_id        string?          认证后有值
  session_epoch    uint32           每次绑定更新递增
  protocol_profile string           编解码配置标识
}
```

- 登录前：target = AuthService，player_id = nil。
- 登录后：target = PlayerService，player_id = 认证结果。
- 认证成功后由 `shield.client.bind` 触发 Gateway 的 CAS 原子更新
  (`apply_binding`:比对 expected epoch → 写 target/player_id → 递增 epoch),
  bind 应答把携带新 epoch 的 `ClientRef` 返回给调用协程。
- 旧 epoch 的 ingress/egress/control 引用必须拒绝或丢弃。
- room/scene/map 的动态路由由 PlayerService 私有状态管理，不经过 Gateway。

## Gateway 路由表

Gateway 维护一张轻量校验表，编译期从 RPC 描述符集构建：

```text
route_id → { direction, requires_auth }
```

- 只做合法性校验：route_id 是否存在、方向是否允许客户端发起、是否需要认证。
- 不含 logical_service_name、handler、schema 或 ServiceAddress。
- 校验失败的消息在 Gateway 直接拒绝，不进入目标 VM。

## 入站

```text
client socket bytes
  → frame decode → 读 header.route_id
  → Gateway 路由表校验（合法 + direction + 认证要求）
  → session.target（AuthService 或 PlayerService）
  → CAF send ClientIngress { gateway_address, session_id, session_epoch, player_id,
                              protocol_profile_id, route_id, body_bytes, decoded_message? }
  → target Service actor mailbox
  → 目标 VM: route_id → cached handler → decode body（或直接消费 decoded_message）→ invoke handler(client, request)
```

route_id 来自 wire header。body_bytes 始终原样传递到目标 VM；当监听器的 protocol pipeline 配置了 codec 插件并完成解码时，Gateway 还会把解码出的规范 JSON message 一并交给 Lua（`on_client_message` 的第 4 个参数，Lua table；没有 codec 插件解码时为 nil）。没有配置 codec 插件时行为与旧设计一致：只转发原始字节，由目标 VM 按 RPC schema 解码。

`ClientIngress` 是 runtime 内部消息，不是 Lua API，也不是客户端 body schema。CAF 负责把该内部消息投递给本地或远端 actor。

## 出站

```text
target Service actor
  → shield.client_rpc.<name> helper（spawn 期按 s2c descriptor 注册，绑定 route_id）
  → CAF send ClientEgress { context, route_id, body_bytes | message } 到 gateway_address
  → Gateway 校验门（按序）：
      1. registry 命中（unknown session）
      2. epoch 相等（stale epoch）
      3. owner player_id 相等（owner mismatch）
      4. descriptor 路由存在（route not found）
      5. 方向允许 server-to-client（direction rejected）
      6. session->send_message（队列满/已关闭 = send failed）
  → route_id 写入 wire header，body_bytes 作为 body
  → frame encode → socket write
```

出站 route_id 来自 helper 已绑定的 server-to-client descriptor;业务也可以传结构化
message(由监听器的 codec 插件编码)。Gateway 不扫描业务参数。

拒绝是 warn + 原子计数(`GatewayStats`),不排队、不重试、不回写错误帧——egress 是
fire-and-forget。写回校验使用 `session_id`、Gateway 地址和 epoch,防止断线、重连或旧
actor 消息写入新的连接。

## Lua 与 Service 边界

- RPC handler 的函数和参数由 RPC 定义/生成 binding 在启动期注册。
- handler 只接收解码后的该 RPC 业务参数；玩家身份来自 runtime 注入的可信 client context。
- 一个在线玩家默认对应一个 PlayerService / CAF actor / 私有 Lua VM。
- 所有客户端消息进入 PlayerService 后，由 PlayerService 内部根据 route_id 决定自己处理还是转发给 room/scene/map。
- `client_disconnect` 是 Gateway 到当前 target 的控制通知,不是业务 RPC 路由入口;
  `shield.client.close` 走 `ClientCloseRequest`:先失效绑定(epoch 递增),再移除注册,
  最后关闭 socket(空 reason 记为 "kicked")。

## 非目标

- 在 Gateway Lua 中按 route/body 做业务二次分发。
- Gateway 多目标路由(session 多服务绑定表,`bind_service`/`unbind_service`)。
- 跨 service 传递 Session/连接句柄(业务只拿只读 `ClientContext`/`ClientRef`)。
- 让 PlayerService 直接操作 socket、codec、frame 或 envelope。
- 框架级 client RPC pending 表、自动 response/future/timeout 关联。
- 以 `PacketKind` 或 seq 作为 Lua 客户端 RPC API。
