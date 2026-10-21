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
                              protocol_profile_id, route_id, body_bytes, decoded_request? }
  → target Service actor mailbox
  → 目标 VM: route_id → cached handler → decode body（或直接消费 decoded_request）→ invoke handler(ctx, client, request)
```

route_id 来自 wire header。body_bytes 始终原样传递到目标 VM；request 值按
descriptor 契约成形：当监听器的 protocol pipeline 配置了 codec 插件并完成解码时，
解码出的规范 JSON message 作为 `ClientIngress.decoded_request` 随行，直接作为
handler 的 request table；否则 request_codec 为空或 `json` 的 route 由目标 VM
把 body_bytes 按 JSON 解码（失败时回退为原始字节字符串）；`raw` 等其他
request_codec 始终传原始字节字符串。

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

## 顶号与重连的时序语义

本节把 epoch CAS 的时序细节显式化：什么操作在什么顺序下发生、在途消息在各
时间窗口内的命运。player 模块的顶号策略（`single` / `kick_old` / `multi`、
重连窗口）是 player 层业务，见 [runtime-player.md](runtime-player.md)；
本节只描述 Gateway 提供的机制与保证。

### epoch CAS 的确切语义

```text
apply_binding(target, player_id, expected_epoch, &out):
    lock(binding_mutex_)
    expected_epoch != binding.epoch 且 != kAnyEpoch → 拒绝（false）
    写 target / player_id → ++epoch → 出参返回新绑定
```

- `session.hpp` 的 `kAnyEpoch = 0xFFFFFFFF` 是无条件替换哨兵，只用于**失效**
  （close 路径），正常 bind 永远带 expected epoch 走 CAS。
- epoch 从 0 开始（建连时 `reset_binding`，target = auth 入口 service）；每次
  bind 成功递增，登录后（auth → player）至少为 1。
- bind 的 CAS 只在 Gateway actor 上发生（`handle_client_bind`），互斥锁保证
  并发 bind 只有一个胜者；败者收到 `client_rpc.epoch_expired`（warn +
  `binds_epoch_expired` 计数，有 call_session 则完成调用并回传失败）。

### 三个检查点，一个不做检查

| 路径 | epoch 校验 | 时序细节 |
|------|-----------|---------|
| bind（ingress 控制） | **CAS**（expected） | 胜者翻转 dispatch：registry 更新完成后才向新 target 发 `Bound` |
| egress（s2c 写回） | **相等** | 携带旧 ClientRef 的 `client_rpc` 一律丢弃（warn + `egress_stale_epoch`），fire-and-forget，无错误帧 |
| close/kick | 无条件（kAnyEpoch） | 失效 → 移除 → 关 socket |
| ingress（c2s 转发） | **不校验** | bridge 在 net 线程对**活绑定做快照**后转发，目标 actor 收到的 context 是快照时刻的 epoch |

ingress 不校验是有意设计：c2s 消息已经进了管道，丢弃它只会制造静默丢失。
真正的保护在**回包侧**——在 bind 翻转前快照进来的 ingress 照常在目标 VM 执行，
但它的 handler 若用旧 ClientRef 发 s2c，egress 门按 epoch 相等拒绝。即：
**旧 epoch 的处理可以完成计算，但旧 epoch 的写回永远到不了 wire**。

### 顶号（kick）的顺序保证

`shield.client.close`（`handle_client_close`）的固定顺序：

1. 先向**当前** target 发 `Unbound`（携带失效前最后一次的 epoch）——必须在
   失效前发，因为 bridge 的 disconnect 回调只通知"还有绑定"的 target；
   kick 必须自补偿这一条，否则业务收不到下线通知。
2. `apply_binding("", "", kAnyEpoch)` 失效绑定——从此刻起所有旧 epoch 引用
   失败，**先于 socket 真正关闭**。
3. registry 移除。
4. `session->close(reason)`；reason 为空记为 `"kicked"`。

对偶保证：bridge 的 `on_disconnect` 看到空绑定即知道 Gateway 已发过 Unbound，
不再补发 `Disconnected`（防止双通知）。**只有非 kick 的自然断线**才走
`Disconnected`（`on_disconnect` 回调、bridge 发送）。

### 重连

- 新 TCP 连接 = 新 session_id、新 Session、epoch 从 0 起。Gateway 不做
  跨连接的 player 归并——**同一 uid 出现两条活 session 在 Gateway 层是合法
  状态**，单玩家唯一性由 player 模块裁决（`single` 拒绝 / `kick_old` 对旧
  session 调 `shield.client.close` / 重连窗口内复用实例重新 bind）。
- 复用 PlayerService 实例的重连：新 session 的 bind（auth → player）走同一
  CAS；旧 session 已断线失效，不存在 epoch 竞争。旧 PlayerService 上残留的
  in-flight egress 会因 epoch 不等被 egress 门丢弃。
- `Reconnected` 控制消息为预留位；当前重连恢复判定在 player 模块内按 uid
  状态完成，不依赖该消息。

### 在途窗口速查

| 事件序列 | 结果 |
|---------|------|
| handler 执行中发生 rebind，handler 用旧 ClientRef 回包 | egress 拒绝（stale epoch），warn + 计数，客户端收不到该帧 |
| bind 胜负同时刻并发 | CAS 单胜者；败者 `client_rpc.epoch_expired`，业务可据此刷新 ClientRef 重试 |
| kick 与自然断线竞争 | 先到者完成 Unbound 通知 + 失效；后到者在 bridge 侧因空绑定直接跳过，无双通知 |
| rebind 后旧 target 的 in-flight ingress 继续执行 | 允许（计算不中断），但其任何 s2c 均被 egress 门拒绝 |

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
