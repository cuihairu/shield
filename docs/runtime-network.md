# 网络运行时语义

本文冻结客户端连接、协议处理和 CAF actor 交互契约。CAF / `shield_core` 是客户端 RPC 入站、出站与 service 消息投递的唯一 actor runtime 边界，本文不保留旧接口或兼容流程。

## 分层职责

| 层 | 职责 | 明确不负责 |
| --- | --- | --- |
| `shield_net` | listener、连接生命周期、I/O、背压、live session 所有权 | 业务 RPC dispatch、玩家状态 |
| `shield_transport` | frame/envelope、header、session 级 codec/profile | 从 body 猜 route、选择业务 actor |
| Gateway runtime | session registry、可信身份、单一 target 绑定、入站/出站校验 | 解普通 RPC body、持有 Lua handler |
| CAF / `shield_core` | actor mailbox、调度、本地/远端消息投递 | 客户端 wire method 到 Lua 函数的自动映射 |
| Service adapter | 客户端 RPC binding、目标 actor 内 decode、Lua handler 调用 | socket、listener、session 所有权 |

CAF middleman 承担 Service actor 的跨进程传输。客户端连接始终归接入它的 Gateway；目标业务 Service 位于本地或远端，不改变上层语义。

## ProtocolProfile

每条 session 在 listener 配置或首阶段握手时绑定一个 `ProtocolProfile`，正常业务阶段不可切换。

Profile 至少确定：

- frame/envelope 格式；
- header 中 `route_id` 的宽度和字节序；
- compiled RPC descriptor；
- body codec 与 schema package；
- frame 大小、解码错误和未知 route 策略。

正常客户端 RPC 必须在 header 中携带 `route_id`。body 只包含该 RPC 的业务参数，不包含 route name、`route_id`、method 或服务名。

## Session 绑定：单一 target

Gateway 为每条 live session 只维护一个当前 target 绑定（`SessionBinding`，
M2 已落地）：

```text
SessionBinding {
  target_service
  player_id?
  gateway_name
  protocol_profile_id
  session_epoch
}
```

规则：

- `session_id` 只在当前 Gateway 内定位连接；`session_epoch` 区分断线重连前后的 owner。
- `player_id` 由认证结果写入，是可信身份，不从客户端业务 body 读取。
- 登录前 target 是认证入口服务；认证成功后由 Gateway actor 以
  compare-and-set 的 epoch 原子替换 target 与 `player_id`（见
  [protocol-routing-design.md](protocol-routing-design.md)）。
- 绑定替换递增 epoch。旧 epoch 的入站、回包和绑定更新必须拒绝或丢弃。
- room、scene、map 等动态路由是 PlayerService 私有业务状态，不进入
  Gateway；session 不存在多服务绑定表，`bind_service`/`unbind_service`
  API 已删除。

## 入站

```text
socket bytes
  -> shield_net
  -> frame/envelope decode
  -> read header.route_id
  -> descriptor 校验表（route 存在、direction、requires_auth）
  -> session 单一 target 绑定
  -> CAF send ClientIngress
  -> target Service actor mailbox
  -> route_id -> 启动期编译的 Lua binding
  -> decode body by profile body codec
  -> invoke handler(ClientContext, request)
```

Gateway 在选择目标 actor 前不得解普通业务 body。这样 header `route_id` 才能用于快速转发，也避免 Gateway 依赖所有业务 schema。

预登录 RPC 在 descriptor 中声明 `requires_auth: false`。认证完成后再由认证服务通过 `shield.client.bind` 把 target 原子切到玩家服务。

## CAF 内部消息

入站使用结构化 runtime 消息，而不是普通 Lua service method：

```text
ClientContextData {
  gateway_address
  session_id
  session_epoch
  player_id?
  protocol_profile_id
}

ClientIngress {
  ClientContextData context
  route_id
  body_codec_name
  body_bytes
  decoded_request?      // profile codec 已解码的规范 JSON（如有）
}
```

`route_id` 是从 wire header 复制的内部 dispatch 元数据，`body_bytes` 仍是原始 RPC body。二者不会重新包装进客户端 body，也不会作为业务参数交给 Lua。完整 CAF 消息契约（`ClientEgress`、`ClientControlMessage`、bind/close 请求）见 [runtime-messaging.md](runtime-messaging.md)。

CAF behavior 至少区分：

- `ClientIngress`；
- `ClientEgress`；
- 普通 Service `send/call`；
- client disconnect/reconnect 等生命周期控制；
- Service lifecycle。

CAF 负责消息类型 dispatch 和 actor 调度；Shield Service adapter 负责 `route_id -> cached Lua handler`。不为每个 RPC 手写一个 CAF C++ 消息类型。

## ClientContext 与 ClientRef

session 连接对象是 Gateway 内部状态,不进入业务 Lua,也不通过 CAF 消息传播。业务只见
只读 `ClientContext` / `ClientRef` userdata(M2 已落地):二者都不暴露 socket、codec、
frame、route、CAF handle 或底层 session 指针,只读属性为
`player_id` / `session_id` / `session_epoch` / `protocol_profile_id` / `gateway`;
`ClientContext` 额外提供 `ref()` 派生值语义 `ClientRef`。

`ClientRef` 封装(M2 已落地):

```text
gateway_address + session_id + session_epoch + player_id + protocol_profile_id
```

它作为普通 service 消息参数传递:传出时序列化为 `__shield_client_ref` JSON 标记,到达
对端 VM 时重新物化为 userdata。Gateway 在处理回包、关闭或绑定更新时重新校验 epoch,
因此旧 `ClientRef` 不会命中新连接。

## 出站

Service 必须通过 spawn 期按 s2c descriptor 自动注册的
`shield.client_rpc.<binding>` helper 发送 response 或 push(M2 已落地):

```text
shield.client_rpc.<name>(ClientContext|ClientRef, business_table_or_bytes)
  -> helper supplies route_id（descriptor 绑定）
  -> table 参数编码为结构化 message，string 参数原样作为 body bytes
  -> CAF send ClientEgress to gateway_address
  -> Gateway 校验门：registry 命中 → epoch 相等 → owner player_id
     → descriptor 路由存在 → 方向允许 s2c → send_message
  -> write route_id into wire header
  -> frame/envelope encode
  -> socket write
```

不存在接受 route 字符串、裸 `route_id` 或通用 table envelope 的业务发送 API。route 元
数据由 helper 绑定的 descriptor 提供,业务参数中出现名为 `route`、`method` 或 `id` 的
字段不会影响分发。egress 是 fire-and-forget:拒绝只 warn + 计数,不排队、不重试、不回写
错误帧。

## 单一 target 绑定

认证服务通过 `shield.client.bind(client, player_id, target)`(M2 已落地)请求 Gateway
actor 原子替换当前 session 的 target 绑定并写入 `player_id`:

- bind 挂起调用协程,Gateway CAS(`apply_binding`:expected epoch 比对 → 写入 →
  epoch 递增)成功后以 `(true, ClientRef)` 恢复,失败以
  `(false, {code="client_rpc.epoch_expired"})` 恢复;
- 旧 `ClientRef` 因 epoch 递增而失效;客户端不能通过 body 指定 target 或修改绑定;
- 关闭当前 client 使用 `shield.client.close(ref, reason)`:先失效绑定,再移除注册,
  最后关闭 socket(空 reason 记为 "kicked");
- room/scene/map 等动态协作是 target 服务私有状态,通过普通
  `shield.send/call` 完成,不经 Gateway。

## 断线与重连

连接断开时，Gateway：

1. 使 live session 失效并递增 epoch；
2. 向当前 target 发送结构化 `ClientControlMessage::Disconnected` 控制消息；
3. 注销该 session 的 registry 记录；
4. 拒绝旧 `ClientRef` 的后续写回。

重连认证成功后，Gateway 创建新 epoch，并由玩家 owner 决定恢复原 PlayerService 还是创建新实例，恢复后重新执行 target 绑定。生命周期控制消息不是客户端 RPC，也不进入通用业务消息入口。

## 背压与限制

网络层必须显式限制：

| 参数 | 说明 |
| --- | --- |
| `max_connections` | listener 最大连接数 |
| `max_connections_per_ip` | 单 IP 最大连接数 |
| `max_frame_size` | 单 frame 最大字节数 |
| `max_session_send_queue` | 单 session 待写队列上限 |
| `max_decode_errors` | 连续协议错误上限 |
| `read_idle_timeout` | 读空闲超时 |
| `write_idle_timeout` | 写空闲超时 |
| `handshake_timeout` | 握手超时 |

`ClientEgress` 被接受只表示进入 Gateway 写回流程，不表示客户端已经收到。队列满、session stale、Gateway 不可达等情况返回明确错误；runtime 不做无界缓存或隐式重试。

## 线程与执行权

- I/O 线程只执行 socket、frame/envelope 和 header 处理，不执行 Lua。
- 目标 Service actor 在自己的 mailbox 调度点执行 handler binding、body decode 和 Lua 调用。
- 同一 Service actor / Lua VM 同时只允许一个执行者。
- 不同 Service actor 由 CAF scheduler 并行调度。
- 同一连接的 frame 顺序在 session strand 上保持；不同连接之间不保证全局顺序。


## Transport 范围

TCP 是第一版必需传输。UDP、KCP、WebSocket 可以作为后续 transport adapter，但必须复用同一 session 单一 target 绑定、header `route_id`、`ClientIngress/ClientEgress` 和 epoch 校验语义，不得为不同 transport 发明不同 Lua 客户端 API。
