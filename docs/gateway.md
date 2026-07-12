# 网关设计

Gateway 是客户端连接、session 管理和 wire 入口的边界，不是业务 Lua 的通用消息分发器。

## 职责

- 持有 live `SessionHandle`，管理连接、断线、重连和写回。
- 维护 session 绑定：target（AuthService 或 PlayerService）、player_id、epoch、protocol profile。
- 只解析 frame/header 所需字段；普通客户端 RPC 只读取 header `route_id`，不解业务 body。
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
- 认证成功后 Gateway 原子更新 target、player_id 和 epoch。
- 旧 epoch 的消息必须拒绝或丢弃。
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
                              protocol_profile_id, route_id, body_bytes }
  → target Service actor mailbox
  → 目标 VM: route_id → cached handler → decode body → invoke handler(client, request)
```

route_id 来自 wire header。body_bytes 原样传递到目标 VM，由目标 VM 按 RPC schema 解码。Gateway 不解码 body。

`ClientIngress` 是 runtime 内部消息，不是 Lua API，也不是客户端 body schema。CAF 负责把该内部消息投递给本地或远端 actor。

## 出站

```text
target Service actor
  → codegen helper 编码业务参数为 body_bytes，绑定 route_id
  → CAF send ClientEgress { session_id, session_epoch, route_id, body_bytes } 到 gateway_address
  → Gateway 校验 session_id + session_epoch
  → route_id 写入 wire header，body_bytes 作为 body
  → frame encode → socket write
```

出站 route_id 来自 codegen helper 已绑定的 server-to-client RPC 描述符。Gateway 不扫描业务参数。

Gateway 的写回校验必须使用 `session_id`、Gateway 地址和 epoch，防止断线、重连或旧 actor 消息写入新的连接。

## Lua 与 Service 边界

- RPC handler 的函数和参数由 RPC 定义/生成 binding 在启动期注册。
- handler 只接收解码后的该 RPC 业务参数；玩家身份来自 runtime 注入的可信 client context。
- 一个在线玩家默认对应一个 PlayerService / CAF actor / 私有 Lua VM。
- 所有客户端消息进入 PlayerService 后，由 PlayerService 内部根据 route_id 决定自己处理还是转发给 room/scene/map。
- `client_disconnect` 是 Gateway 到当前 PlayerService 的控制通知，不是业务 RPC 路由入口。

## 非目标

- 在 Gateway Lua 中按 route/body 做业务二次分发。
- Gateway 多目标路由（session.service_routes[room/scene/map]）。
- 跨 service 传递 `SessionHandle`。
- 让 PlayerService 直接操作 socket、codec、frame 或 envelope。
- 框架级 client RPC pending 表、自动 response/future/timeout 关联。
- 以 `PacketKind` 或 seq 作为 Lua 客户端 RPC API。
