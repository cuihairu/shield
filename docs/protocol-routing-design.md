# 客户端 RPC 服务自治路由设计

> 状态：已实现。本文是客户端 RPC 路由、body 编解码和
> Gateway/Service 边界的唯一设计依据；与旧 `LuaGatewayBridge`、
> `on_client_message`、`session.target` 或 body-route 配置相冲突的描述均为
> 待删除遗留，不是兼容目标。

## 决策

客户端 wire header 只携带 `route_id` 等传输字段，body 只携带该 RPC 的业务
数据。`route_id` 从 compiled RPC descriptor 找到**逻辑服务名**；Gateway 用
session 的动态绑定把逻辑服务名解析为当前 ServiceAddress。目标 Service 独占
该服务的 RPC handler 表、请求/响应 schema 和 body codec。

```text
client bytes
  -> Gateway: envelope decode + header.route_id + edge validation
  -> RpcDescriptor: route_id -> logical_service
  -> SessionRoutingContext: logical_service -> ServiceAddress
  -> ClientIngress(raw body bytes, ClientRef, descriptor identity)
  -> target Service RPC adapter: route_id -> cached binding -> body decode
  -> Lua handler(ClientContext, request)
```

这不是“Gateway 业务分发”：Gateway 不知道 Lua handler、schema 或具体 codec，
也不根据 body 作选择。它只拥有 live session、可信身份、动态服务绑定和客户端
wire 边界。服务可独立拥有自己的 RPC 路由，因此 Player、Room、Scene、Map 等
服务可按 descriptor 注册各自的路由，不需要把业务路由集中到 Gateway 或一个
PlayerService。

## 术语与所有权

| 对象 | Owner | 内容 | 不包含 |
| --- | --- | --- | --- |
| `ProtocolProfile` | Gateway/transport | envelope、header `route_id` 格式、descriptor package identity、传输限制 | body route、Lua handler、schema 实现 |
| `RpcMethodDescriptor` | descriptor/toolchain | `route_id`、direction、logical_service、schema identity、binding hint、edge policy | ServiceAddress、SessionHandle |
| `SessionRoutingContext` | Gateway | session id/epoch、player identity、profile、`logical_service -> ServiceAddress` | 全局 service registry、业务 handler |
| `ServiceRpcTable` | target Service | 本服务允许的 route、缓存 Lua binding、request/response codec/schema | socket、live session |
| `ClientContext` / `ClientRef` | Service adapter | 可信 client identity 和 Gateway 回包地址 | SessionHandle、frame、codec 实现 |

`logical_service` 是 descriptor 中稳定的领域名称，例如 `auth`、`player`、
`room`、`scene`、`map`；不是 actor name，不是实例 id，也不进入客户端 body。
Gateway 只在当前 session 的 `service_routes` 中查它，不能回退到全局 registry。

## Descriptor 是唯一静态来源

每个 RPC descriptor 至少包含：

```text
RpcMethodDescriptor {
  route_id          uint32
  full_name         string
  direction         client_to_server | server_to_client
  logical_service   string
  request_schema    SchemaRef?       // client_to_server
  response_schema   SchemaRef?       // server_to_client
  binding_hint      string           // target Lua method
  requires_auth     bool
  max_body_size     uint32
}
```

约束：

- `route_id` 在一个 `ProtocolProfile` 中唯一，且只能来自 wire header。
- `logical_service`、方向、认证要求、schema 和 binding 只在 descriptor 定义；
  不能在 `actors[].network.protocol.routes[]`、Lua table 或 body 中复制一份。
- target Service 启动时只加载属于自己的 descriptor entries，编译为
  `route_id -> cached Lua function + schema codec`。缺失、重复或方向不匹配的
  binding 必须在启动期失败。
- `route`、`method`、`route_id`、`msg_id` 等字段即使出现在业务 JSON 中，也只
  是业务字段，绝不能影响 Gateway 或 Service 的 RPC 选择。

## 入站路径

```text
socket bytes
  -> envelope decode
  -> read header.route_id
  -> descriptor lookup
  -> Gateway edge validation
       direction, requires_auth, frame/body size, session epoch
  -> session.service_routes[descriptor.logical_service]
  -> CAF ClientIngress
  -> target ServiceRpcTable lookup
  -> request schema / codec decode
  -> Lua binding(ClientContext, decoded request)
```

Gateway 的 edge validation 只使用 descriptor 投影出的轻量元数据；它不调用 body
codec。未知 route、客户端发送 server-to-client route、未认证访问、缺少当前服务
绑定、过期 epoch 都在 Gateway 拒绝。body 不符合 schema、handler 执行失败等由
目标 Service 报告为 RPC 处理错误。

`ClientIngress` 是类型化 runtime 消息，而不是普通 service method：

```text
ClientIngress {
  ClientRef client
  ProtocolProfileId profile_id
  DescriptorId descriptor_id
  uint32 route_id
  ByteBuffer body_bytes
}
```

它不携带解码后的 JSON、route name、Lua handler、SessionHandle 或 raw frame。

## 服务自治路由

Gateway 维护的不是单一 `session.target`，而是当前连接最小必要的动态服务表：

```text
SessionRoutingContext {
  gateway_address
  session_id
  session_epoch
  player_id?
  protocol_profile_id
  service_routes: {
    auth   -> AuthServiceAddress,
    player -> PlayerServiceAddress,
    room   -> RoomServiceAddress?,
    scene  -> SceneServiceAddress?,
    map    -> MapServiceAddress?
  }
}
```

- 新连接仅安装受限的 `auth` bootstrap binding。
- 认证服务不能直接修改 `SessionHandle`；它向 Gateway 发送带 `ClientRef` 的类型化
  control message，以 compare-and-set 的 epoch 原子绑定 `player_id` 和 `player`。
- Player/Room/Scene/Map 等获授权服务可以同样绑定、替换或解除自己负责的逻辑服务。
  Gateway 校验调用者权限、Gateway 地址和 epoch 后更新表。
- 任一次绑定变更递增 epoch；旧 ingress、egress 与 control message 必须失效。
- 服务之间的后续协作仍使用普通 `shield.send/call`；是否转发到 room/scene/map 是
  各服务的业务决定，不通过 Gateway Lua 回调实现。

这让“哪个服务接收某一类客户端 RPC”由 descriptor 明确，而“该 session 当前由
哪个实例服务”由 Gateway 的动态绑定明确，两者都不依赖客户端可控数据。

## 编解码边界

`shield_transport` 只负责字节流、envelope、header 和 frame 限制。body codec 是
descriptor 驱动的 Service RPC adapter 能力：

```text
inbound:  ClientIngress.body_bytes -> service-selected codec/schema -> Lua request
outbound: Lua response -> service-selected codec/schema -> ClientEgress.body_bytes
```

- protobuf、msgpack 等 provider 只在 target Service 的 descriptor/schema 绑定中
  被解析。插件仍可通过稳定 C ABI 返回 bytes/JSON bridge，但不参与 Gateway 的
  handler dispatch。
- codec 不能从 body 抽取或猜测 route；body codec 的输入 route 已由 descriptor
  绑定，输出只表示业务 body。
- 一个 session 的 envelope/profile 可以固定；同一服务的不同 RPC 可以使用其
  descriptor 声明的 schema codec。是否允许多 codec 是 descriptor/toolchain 的
  显式能力，绝不通过 body 自动探测。
- raw forwarding 是独立数据面能力，必须有显式 destination 和 ownership；它不是
  `ForwardRaw` 后静默丢弃，也不是普通 Lua RPC 的回退路径。

## 出站路径

业务代码只能使用 codegen 生成的具体 server-to-client RPC helper：

```text
Lua handler
  -> generated helper(ClientContext | ClientRef, business arguments)
  -> descriptor binds route_id + response schema + codec
  -> encode body bytes in target Service
  -> CAF ClientEgress
  -> Gateway validates client gateway/session/epoch/owner
  -> envelope writes route_id header
  -> socket write
```

```text
ClientEgress {
  ClientRef client
  ProtocolProfileId profile_id
  uint32 route_id
  ByteBuffer body_bytes
}
```

Gateway 不从 response table 的 `route_id`、`route`、`method` 或 `msg_id` 推断
route。普通业务 Service 不获得 `SessionHandle`，不直接调用 `session:send`，不接触
frame、envelope 或 `ProtocolPipeline`。

## 生命周期与错误

- Gateway 关闭 session 后先使当前 epoch 失效，再向当前绑定服务发送类型化
  `ClientDisconnected` 控制消息；它不是 Lua `on_disconnect` 业务回调。
- `ClientEgress` 入队成功不代表客户端收到数据。队列满、gateway 不匹配、session
  不存在或 epoch 过期必须返回稳定错误，不能隐式重试或缓存。
- 协议/编码错误按边界归属：frame/header 错误由 Gateway 处理；request/response
  schema 或 codec 错误由目标 Service 处理；route/binding 配置错误启动期失败。

## 禁止项

- 单一 `session.target` 作为所有业务 RPC 的唯一目标。
- Gateway Lua `on_client_message` 作为正式 RPC dispatch 入口。
- `actors[].network.protocol.routing`、`routes[]`、`action`、`lazy_decode`、
  `routing.source` 或 body route 规则。
- Gateway 提前解普通 RPC body，或将 canonical JSON 作为 ingress 的旁路参数。
- 业务通过通用 `SessionHandle:send`、裸 route id、route 字符串或 envelope table 回包。
- 从业务 body 的字段推断 inbound/outbound route。
- 跨 service 或跨节点传递 `SessionHandle`。

## 验收标准

实现完成至少需要以下端到端验证：

1. 真实 TCP 客户端经 header route 到 AuthService，body 仅在 AuthService 解码。
2. 认证以 epoch CAS 原子建立 `player` binding；旧 ClientRef 无法写回。
3. 同一 session 的 `player` 与 `room` route 分别送达其当前 ServiceAddress，且
   Gateway 不依赖 Lua handler/schema。
4. 目标服务通过生成 helper 发送 protobuf/msgpack response；Gateway 只封装 header
   并写回 socket。
5. 缺失 descriptor、重复 binding、无 session service binding、方向/认证错误、
   schema 错误、stale egress 和写队列满均覆盖稳定错误路径。
