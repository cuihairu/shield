# 客户端 RPC 服务自治路由设计

> 状态：descriptor 契约与启动期 binding 编译已实现（架构纠偏 M1）。
> session 单一 target 绑定、`ClientContext`/`ClientRef`、typed
> `ClientIngress`/`ClientEgress` 按路线图 M2/M3 落地；文中相应小节标注了
> 责任里程碑。本文是客户端 RPC 路由、body 编解码和 Gateway/Service 边界的
> 唯一设计依据；与旧 `LuaGatewayBridge` body-route、`on_client_message`
> 常驻分发或 `service_routes` 多绑定相冲突的描述均为待删除遗留，不是兼容
> 目标。

## 决策

客户端 wire header 只携带 `route_id` 等传输字段，body 只携带该 RPC 的业务
数据。`route_id` 的唯一静态来源是 **compiled RPC descriptor**，由各 actor
在自己的 `actors[].rpc.routes` 中声明，bootstrap 合并为全局 descriptor
表；每个 VM 启动时只编译属于自己的条目。一条 session 在 Gateway 只有一个
**单一 target** 绑定：认证前指向 AuthService，认证成功后原子切换到
PlayerService（epoch 递增）；room/scene/map 等动态路由是 PlayerService 的
私有业务状态，通过普通 `shield.send/call` 协作，不经过 Gateway。

```text
client bytes
  -> Gateway: envelope decode + header.route_id + edge validation
  -> session 单一 target 绑定
  -> ClientIngress(typed, 携带可信 client 上下文)          [M3]
  -> target Service: route_id -> 启动期编译的 Lua binding
  -> Lua handler(ClientContext, request)
```

这不是“Gateway 业务分发”：Gateway 不知道 Lua handler、schema 或具体 codec，
也不根据 body 作选择。它只拥有 live session、可信身份和单一 target 绑定。
descriptor 明确“哪个 route 由哪个 actor 的哪个 Lua 方法处理”；session 绑定
明确“该 session 当前由哪个实例服务”，两者都不依赖客户端可控数据。

## 术语与所有权

| 对象 | Owner | 内容 | 不包含 |
| --- | --- | --- | --- |
| `ProtocolProfile` | Gateway/transport | envelope、header `route_id` 格式、传输限制 | body route、Lua handler、schema 实现 |
| `RpcDescriptor` | descriptor/bootstrap | `route_id`、direction、`binding`、`owner_service`、auth/policy 元数据 | ServiceAddress、socket、live session |
| Session 单一 target 绑定 | Gateway | target service、player_id、epoch、profile [M2] | 全局 service registry、业务 handler |
| 每 VM RPC 表 | target Service | 本服务拥有的 route -> 已编译 `sol::function` | socket、listener、session 所有权 |
| `ClientContext` / `ClientRef` | Service adapter | 可信 client identity 和 Gateway 回包地址 [M2] | SessionHandle、frame、codec 实现 |

## Descriptor 是唯一静态来源

每个 RPC descriptor 在 actor 配置 `actors[].rpc.routes` 中声明：

```yaml
actors:
  - name: auth_gateway
    rpc:
      routes:
        - id: 1001
          name: login            # 仅日志/排错，不进 wire
          binding: do_login      # c2s/bidi: 本 actor 的 Lua 方法名
          direction: c2s         # c2s | s2c | bidi
          requires_auth: false
        - id: 2001
          name: login_result
          binding: push_result   # s2c: 出站 helper 名
          direction: s2c
```

字段与默认值：

| 字段 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `id` | int >= 1 | 必填 | wire 上的 `route_id`，全局唯一 |
| `name` | string | 空 | 诊断名，全局唯一（如声明）；不进 wire |
| `binding` | string 非空 | 必填 | c2s/bidi = Lua 方法名；s2c = 出站 helper 名 |
| `direction` | enum | `c2s` | `c2s`/`client_to_server`、`s2c`/`server_to_client`、`bidi`/`bidirectional` |
| `owner_service` | string | 本 actor | 拥有该 binding 的 actor 名；只有 owner 在 spawn 时编译它 |
| `requires_auth` | bool | `true` | Gateway 边缘认证校验 |
| `action` | enum | `decode_local` | `decode_local` / `forward_raw` / `drop` |
| `lazy_decode` | bool | `true` | 解码时机 |
| `request_codec` / `request_schema` / `response_schema` | string | 空 | schema 元数据，Phase 1 仅记录 |

校验分三层，全部失败即启动失败：

1. **config 校验**（config.cpp）：字段类型、枚举、`id >= 1`、actor 内
   `id`/`name` 唯一、`binding` 非空。
2. **bootstrap 合并**：所有 actor 的 routes 并集；跨 actor 的 `id`/`name`
   冲突（`rpc route conflict: id N`）导致启动失败。合并结果注入 listener
   的 `ProtocolBuildOptions::descriptor_routes`，派生 pipeline 内的
   RouteTable。
3. **spawn 编译**（lua_service.cpp）：每个 VM 只加载
   `owner_service == 自身` 的条目；c2s/bidi 的 `binding` 必须解析为该
   service 模块函数，否则 spawn 失败（`handler_missing`）；s2c 只要求
   `binding` 非空（helper 由 Lua API 层在 M2 注册）。

约束：

- `route_id` 只能来自 wire header；`network.protocol.routes` 内联路由已
  删除，配置中出现即报错（pre-1.0 不做兼容读）。
- 方向、认证要求、schema 与 binding 只在 descriptor 定义；不能在 Lua
  table 或 body 中复制一份。
- `route`、`method`、`route_id`、`msg_id` 等字段即使出现在业务 JSON 中，
  也只是业务字段，绝不能影响 Gateway 或 Service 的 RPC 选择。

## 入站路径

```text
socket bytes
  -> envelope decode
  -> read header.route_id
  -> descriptor 校验表（route 存在、direction 允许客户端发起、requires_auth）
  -> session 单一 target 绑定 [M2]
  -> CAF ClientIngress（typed，含可信 client 上下文） [M3]
  -> target VM: route_id -> 启动期编译的 binding
  -> request decode（profile body codec）
  -> handler(ClientContext, decoded request)
```

Gateway 的 edge validation 只使用 descriptor 投影出的轻量元数据；它不解
body。未知 route、客户端发送 server-to-client route、未认证访问都在
Gateway 拒绝（丢帧 + warn + 计数，不回写错误帧）；handler 执行失败由目标
Service 以稳定错误码报告（见“生命周期与错误”）。

`ClientIngress` 是类型化 runtime 消息，而不是普通 service method；契约见
[runtime-messaging.md](runtime-messaging.md)。过渡期（M1 已完成、M3
翻转前）入站仍走 `on_client_message` JSON 约定入口，目标取 session 当前
target；M3 删除该入口。

## Session 绑定：单一 target [M2]

Gateway 为每条 live session 保存一个当前 target 绑定：

```text
SessionBinding {
  target_service        // 认证前 AuthService；认证后 PlayerService
  player_id             // 认证前为空
  gateway_name
  protocol_profile_id
  epoch                 // 每次绑定替换递增
}
```

- 新连接的初始 target 是 listener 配置的认证入口服务。
- 认证服务不能直接改 session；它通过 `shield.client.bind(client,
  player_id, target)` 发送类型化 bind 请求到 Gateway actor，Gateway 以
  compare-and-set 的 epoch 原子替换 target 与 player_id，并返回新的
  `ClientRef`。
- 任何绑定变更递增 epoch；旧 ingress、egress 与 control message 全部失效。
- room/scene/map 等动态路由是 PlayerService 私有状态；服务间协作使用普通
  `shield.send/call`。不存在 `bind_service`/`unbind_service` 多绑定 API。
- “这个玩家当前在哪个 room”由 PlayerService 自己记录并转发，不通过
  Gateway Lua 回调实现。

## 编解码边界

`shield_transport` 只负责字节流、envelope、header 和 frame 限制。body
codec 是 profile 级能力（`network.protocol.body`），解码发生在目标 Service
actor 内：

```text
inbound:  ClientIngress.body_bytes -> profile codec -> Lua request（或启动
          期已解码的 decoded_request 旁路参数）
outbound: Lua response -> ClientEgress.body_bytes
```

- descriptor 的 `request_schema`/`response_schema` 在 Phase 1 只作为元数据
  记录；按 schema 选择 codec 的 provider 机制属于后续 toolchain 方向，
  插件仍可通过稳定 C ABI 提供 bytes/JSON bridge，但不参与 handler
  dispatch。
- codec 不能从 body 抽取或猜测 route；body codec 的输入 route 已由
  descriptor 绑定，输出只表示业务 body。
- raw forwarding（`action: forward_raw`）必须有显式 ownership；它不是
  普通 Lua RPC 的回退路径，也不会静默丢弃。

## 出站路径 [M2]

业务代码只能使用按 s2c descriptor 自动注册的具体 helper：

```text
Lua handler
  -> shield.client_rpc.<name>(ClientContext | ClientRef, business arguments)
  -> descriptor 绑定 route_id
  -> encode body bytes in target Service
  -> CAF ClientEgress
  -> Gateway 校验 registry 命中、session 存活、epoch 相等、owner player_id、direction
  -> envelope writes route_id header
  -> socket write
```

Gateway 不从 response table 的 `route_id`、`route`、`method` 或 `msg_id`
推断 route。普通业务 Service 不获得 `SessionHandle`，不直接调用
`session:send`，不接触 frame、envelope 或 `ProtocolPipeline`。
`ClientEgress` 是 fire-and-forget：入队成功只表示进入 Gateway 写回流程。

## 生命周期与错误

- Gateway 关闭 session 后先使当前 epoch 失效，再向当前 target 发送类型化
  `ClientControlMessage::Disconnected` [M2]；它不是 Lua `on_disconnect`
  业务回调。
- 稳定错误码：`client_rpc.not_authenticated`、`route_not_found`、
  `direction_rejected`、`epoch_expired`、`handler_missing`。入站校验失败
  = 丢帧 + warn + 计数，不回写错误帧。
- `ClientEgress` 被接受不代表客户端收到数据。队列满、gateway 不匹配、
  session 不存在或 epoch 过期按错误码拒绝，不隐式重试或缓存。
- 协议/编码错误按边界归属：frame/header 错误由 Gateway 处理；request
  decode 错误由目标 Service 处理；route/binding 配置错误启动期失败。

## 禁止项

- Gateway 按 body 内容、route 字符串或 Lua 回调做业务二次分发（M3 删除
  `on_client_message` 常驻入口）。
- session 多服务绑定表（`service_routes`）、`bind_service`/
  `unbind_service` API。
- `network.protocol.routes` 内联路由，或从业务 body 字段推断
  inbound/outbound route。
- Gateway 提前解普通 RPC body，或将 canonical JSON 作为 ingress 的旁路
  参数绕过目标 Service。
- 业务通过裸 route id、envelope table 或 `session:send` 回包。
- 跨 service 或跨节点传递 `SessionHandle`。

## 验收标准

以 [roadmap](roadmap.md) 架构纠偏 3-11 的验收契约为准；端到端闭环
（真实 TCP 客户端 login -> bind -> move -> s2c 回包、header route_id 正
确、body 纯业务）在 M6 的 acceptance 测试中落地。
