# 网络运行时语义

本文冻结客户端连接、协议处理和 CAF actor 交互契约。CAF / `shield_core` 是客户端 RPC 入站、出站与 service 消息投递的唯一 actor runtime 边界，本文不保留旧接口或兼容流程。

## 分层职责

| 层 | 职责 | 明确不负责 |
| --- | --- | --- |
| `shield_net` | listener、连接生命周期、I/O、背压、live session 所有权 | 业务 RPC dispatch、玩家状态 |
| `shield_transport` | frame/envelope、header、session 级 codec/profile | 从 body 猜 route、选择业务 actor |
| Gateway runtime | session registry、可信身份、动态服务路由、入站/出站校验 | 解普通 RPC body、持有 Lua handler |
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

## SessionRoutingContext

Gateway 为每条 live session 保存：

```text
SessionRoutingContext {
  gateway_address
  session_id
  session_epoch
  player_id?
  protocol_profile_id
  service_routes: map<logical service name, ServiceAddress>
}
```

规则：

- `session_id` 只在当前 Gateway 内定位连接；`session_epoch` 区分断线重连前后的 owner。
- `player_id` 由认证结果写入，是可信身份，不从客户端业务 body 读取。
- 登录后必须原子建立 `player_id` 和 `player -> PlayerServiceAddress`。
- `scene`、`room`、`map` 等绑定只记录该玩家当前实际关联的 actor。
- 逻辑服务名来自 RPC descriptor，不是固定 actor id；实际地址来自当前 session。
- 更新或移除服务绑定时递增 epoch。旧 epoch 的入站、回包和路由更新必须拒绝或丢弃。

session 不复制全局 Service registry。它只保存当前连接需要的动态路由。

## 入站

```text
socket bytes
  -> shield_net
  -> frame/envelope decode
  -> read header.route_id
  -> RpcMethodDescriptor lookup
  -> validate direction/auth/rate/size
  -> descriptor.logical_service_name
  -> session.service_routes[name]
  -> CAF send ClientIngress
  -> target Service actor mailbox
  -> route_id -> cached Lua handler
  -> decode body by RPC request schema
  -> invoke handler(ClientContext, request)
```

Gateway 在选择目标 actor 前不得解普通业务 body。这样 header `route_id` 才能用于快速转发，也避免 Gateway 依赖所有业务 schema。

预登录 RPC 的 descriptor 可以声明 `auth` 或 `gateway` 逻辑服务。listener 在创建 session 时为这些入口安装受限的 bootstrap route；认证完成后再绑定默认 `player`。

## CAF 内部消息

入站使用结构化 runtime 消息，而不是普通 Lua service method：

```text
ClientIngress {
  gateway_address
  session_id
  session_epoch
  player_id?
  protocol_profile_id
  route_id
  body_bytes
}
```

`route_id` 是从 wire header 复制的内部 dispatch 元数据，`body_bytes` 仍是原始 RPC body。二者不会重新包装进客户端 body，也不会作为业务参数交给 Lua。

CAF behavior 至少区分：

- `ClientIngress`；
- `ClientEgress`；
- 普通 Service `send/call`；
- client disconnect/reconnect 等生命周期控制；
- Service lifecycle。

CAF 负责消息类型 dispatch 和 actor 调度；Shield Service adapter 负责 `route_id -> cached Lua handler`。不为每个 RPC 手写一个 CAF C++ 消息类型。

## ClientContext 与 ClientRef

`SessionHandle` 是 Gateway 内部连接对象，不进入业务 Lua，也不通过 CAF 消息传播。

RPC handler 收到只读 `ClientContext`。它提供可信 `player_id` 和当前 client identity，并可导出值语义 `ClientRef`。二者都不暴露 socket、codec、frame、route、CAF handle 或底层 session 指针。

`ClientRef` 至少封装：

```text
gateway_address + session_id + session_epoch + player_id? + protocol_profile_id
```

它可以作为普通 service 消息参数传递或跨进程序列化。Gateway 在处理回包、关闭或路由更新时必须重新校验 epoch，因此旧 `ClientRef` 不会命中新连接。

## 出站

Service 必须通过已注册的 server-to-client RPC helper 发送 response 或 push：

```text
generated RPC helper(ClientContext|ClientRef, business arguments)
  -> method descriptor supplies route_id and response schema
  -> encode business arguments
  -> CAF send ClientEgress to gateway_address
  -> Gateway validates session_id + session_epoch + owner
  -> write route_id into wire header
  -> frame/envelope encode
  -> socket write
```

不存在接受 route 字符串、裸 `route_id` 或通用 table envelope 的业务发送 API。route 元数据由 helper 绑定的 descriptor 提供，业务参数中出现名为 `route`、`method` 或 `id` 的字段不会影响分发。

## 动态服务绑定

认证服务和已授权业务 Service 可以通过 client routing API 更新当前 session：

- 原子绑定 `player_id` 与默认 `player` Service；
- 绑定或替换 `scene`、`room`、`map` 等逻辑服务；
- 离开目标时解除对应绑定；
- 关闭当前 client。

每次操作都携带 `ClientContext` 或 `ClientRef`，由 Gateway 做 epoch 和权限校验。客户端不能通过 body 指定 ServiceAddress 或修改路由表。

## 断线与重连

连接断开时，Gateway：

1. 使 live session 失效并递增 epoch；
2. 向当前 `player` Service 发送结构化 `ClientDisconnected` 控制消息；
3. 清理或冻结该 session 的动态服务路由；
4. 拒绝旧 `ClientRef` 的后续写回。

重连认证成功后，Gateway 创建新 epoch，并由玩家 owner 决定恢复原 PlayerService 还是创建新实例。恢复成功后再重建其他动态路由。生命周期控制消息不是客户端 RPC，也不进入通用业务消息入口。

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

TCP 是第一版必需传输。UDP、KCP、WebSocket 可以作为后续 transport adapter，但必须复用同一 `SessionRoutingContext`、header `route_id`、`ClientIngress/ClientEgress` 和 epoch 校验语义，不得为不同 transport 发明不同 Lua 客户端 API。
