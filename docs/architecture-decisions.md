# 架构决策记录

本文档冻结 Shield 当前阶段的架构决策，作为「设计说了什么」与「实现做到什么」的对齐基准。决策一旦写入本文，后续实现偏离即视为架构债，需显式纠偏而非默认继承。

若本文与 [架构总纲](architecture.md)、[核心设计理念](architecture-core-concepts.md) 或 [Skynet 对比](skynet-comparison.md) 冲突，以本文为准。

---

## 背景：设计目标与当前实现的偏差

Shield 最初的设计约定是：

> **参考 skynet 的 service/call/send/coroutine 语义，用 CAF 承接 actor runtime，Service 是唯一的可寻址 RPC 单元。**

见 [核心设计理念 §3](architecture-core-concepts.md)、[Skynet 对比 §CAF 与 Skynet 语义的关系](skynet-comparison.md)。

但当前实现**没有完整落地这条约定**：

- `CafAdapter::call` 仍是 stub：`src/core/caf_adapter.cpp:94-105`（注释自认 "For now, return a dummy response"）。
- `CafAdapter::spawn_service` 的 actor behavior 只是占位：`src/core/caf_adapter.cpp:70-72`（"Message dispatch will be replaced by Shield envelope routing"）。
- bootstrap 创建 Lua 服务时走的是 `lua_services->spawn(...)`，不经 `caf_adapter->spawn_service`：`src/bootstrap/bootstrap.cpp:365`。
- Lua 服务实际由 `LuaServiceManager` 自管一套 mailbox + 单 worker runtime：`src/lua/lua_service.cpp:35-112, 829-929`。

也就是说，当前存在 **CAF actor 路径（未闭环）** 与 **LuaServiceManager 自管 runtime（实际在用）** 两套并行的 service runtime。这不是合理分层，而是「未完成的 CAF 路径 + 为了先跑通而长出来的临时 runtime」。

本文冻结的决策，意在先把「坚持原设计」这一点定死，再把客户端交互契约和玩家承载模型收口，避免后续在偏差地基上继续堆叠。

---

## AD-01：CAF 是唯一 actor runtime

**决策**：Service 必须建立在 CAF actor 语义之上；`spawn / send / call / exit` 收口到 `shield_core`，不再在 Lua 层另起一套独立 runtime。

**理由**：这是项目最初的约定，也是「不重复造 skynet」的核心。CAF 提供了 actor / mailbox / scheduler / request-reply / remote actor，完全够用；选型没有错。

**当前状态**：`LuaServiceManager` 自管 runtime 属于**架构偏差**，不是既定形态。纳入纠偏计划。

**纠偏顺序**（见 [roadmap](roadmap.md)）：
1. `dispatch_stack` thread-local 化 + 服务注册表加 shared_mutex（零语义变化）。
2. `shield.call` 从同步重入改为 coroutine yield + mailbox reply。
3. per-service 执行权 + 全局激活队列 + N worker 抢占调度（达成 skynet 式多核并发，每 VM 串行）。

---

## AD-02：Service 是唯一可寻址 RPC 单元

**决策**：

- 只有 Service 拥有 mailbox、CAF address、`send / call` 能力。
- 一个在线玩家 = 一个 PlayerService = 一个 CAF Actor = 一个私有 Lua VM。
- 玩家业务状态是该 service 私有 Lua table，不另起运行时对象。

**理由**：严格保持 skynet 模型——`send / call` 的目标永远是 Service，不是 Entity、不是玩家对象。这样 owner 边界最清晰，并发模型最简单，调试最容易。

**含义**：

- 跨服务调用：`shield.send(service, method, ...)` / `shield.call(service, method, ...)`。
- 客户端 RPC 的 `route_id` 先由描述符选择逻辑服务名；Gateway 再从该 session 的动态路由上下文解析实际 Service actor。逻辑服务名默认是 `player`，也可以是 `scene`、`room`、`map` 等当前玩家已绑定的服务。
- 目标 Service adapter 在本 VM 内按 header `route_id` 直达启动期注册的 handler。业务 Lua 不接收通用客户端消息后再自行分发。

---

## AD-03：不引入 Entity 运行时

**决策**：第一阶段**不**做以下任何一项：

- `PlayerEntity` / `AvatarEntity`
- Entity mailbox
- Entity `send / call` / Entity RPC
- 全局可写 `EntityManager.get(id)`
- `AvatarShard` / `player_pool` 分片承载

普通数据（房间、战斗、怪物、掉落物等）作为所属 service 内部 Lua table；只有确实需要独立 actor 语义的对象才成为独立 service。

**理由**：

- 引入「可 `send/call` 的 PlayerEntity」等于在 Service 这层 actor runtime 之上再叠一层 actor runtime，复杂度爆炸，owner/mailbox/timer/coroutine/RPC 全部出现两套归属。
- KBEngine 的 Entity 之所以能 RPC，是因为它的 Entity 本身就是「带领域语义的 actor-like 对象」，背后有完整运行时；那是另一条路线，不在本阶段采用。
- 第一版优先把 service/call/runtime 语义做对、做稳，降低理解成本和出错面。

**未来**：若真需要「Entity 即轻量 actor」，作为一次**明确的架构升级**引入，不预埋、不污染当前 API。

参见 [实体与组件化草案](runtime-entity.md)（该文已降级为草案/非当前契约）。

---

## AD-04：one-player-one-service 为默认容量模型

**决策**：默认每个在线玩家对应一个 PlayerService（独立 Lua VM）。不为未测到的内存压力提前做 shard / pool 优化。

**理由**：

- 这是 skynet agent 模型的默认形态，owner 最清晰、重连/登出/保存/定时器都挂在同一处。
- 内存取舍经评估可接受：每玩家 VM 的实际成本需用真实压测定，而不是凭空假设后提前优化。
- 提前引入 shard 会把「玩家路由、owner 校验、shard 内状态管理」等复杂度前置，违背「先把架构做对」的原则。

**未来**：仅在真实压测证明每玩家 VM 成本不可接受时，才引入 `player_pool / avatar_shard`，且作为独立架构升级项，不改 public API。

---

## AD-05：客户端 RPC 路由设计

**决策**：wire header 携带 `route_id`，body 是纯业务数据。所有客户端消息经 Gateway 发给 session 绑定的目标服务（登录前 → AuthService，登录后 → PlayerService）。`route_id` 的唯一职责是在目标 Service VM 内命中启动期缓存的 Lua handler。Gateway 不从 `route_id` 解析目标服务，不做多目标路由。

**wire 格式**：

```text
+------------------+------------------+
|     Header       |       Body       |
| (route_id, ...)  | (业务数据)        |
+------------------+------------------+
```

Header 携带 `route_id` 等传输层字段。Body 是纯业务数据，编码格式由 ProtocolProfile 决定（json / protobuf / msgpack）。route_id 不出现在 body 中。

**入站路径**：

```text
socket bytes
  → frame decode → 读 header.route_id
  → Gateway 路由表校验（route_id 合法 + direction + 认证要求）
  → session.target（AuthService 或 PlayerService）
  → CAF send ClientIngress { session_id, epoch, player_id, route_id, body_bytes }
  → 目标 VM: route_id → cached handler → decode body → invoke handler(client, request)
```

- Gateway 只读 header 中的 `route_id` 做合法性校验，不解码 body。
- `body_bytes` 原样传递到目标 VM，由目标 VM 按 RPC schema 解码。
- handler 签名固定为 `handler(ClientContext, decoded_request)`，不含 route_id 或原始 frame。

**预登录路由**：认证前 session 尚未绑定 PlayerService，预登录 RPC（login、token 验证等）目标为 AuthService，使用同样的 route_id → handler 机制。认证成功后 Gateway 原子切换 session.target = PlayerService。

**出站路径**：codegen 生成的 server-to-client helper 已绑定 route_id 和 response_schema。业务只传 ClientContext + 业务参数。helper 编码 body_bytes 后 CAF 发送 ClientEgress 到 Gateway，Gateway 把 route_id 写入 wire header，body_bytes 作为 body。

**codec 与路由无关**：codec 是 session 级固定的（`src/transport/protocol.cpp:1669,1719`），不按 route 选择。`codec_for_route()` 忽略 route，恒返回 default_codec_id（`src/transport/protocol.cpp:1510-1517`）。

**RPC 描述符**：每个客户端 RPC 在编译期由描述符定义 route_id、full_name、direction、request_schema、response_schema、binding_hint。目标 Service 启动时编译为 `route_id → cached Lua handler` 映射。route_name 只用于日志和调试，不进入 wire。

**请求、响应与 push 边界**：

- Shield 提供 descriptor 驱动的客户端 RPC method dispatch，以及生成的 server-to-client response/push helper。
- 第一版不提供通用 `client.call`、seq pending map、future 或自动超时关联。若某个 RPC 需要 correlation token，由该 RPC 契约显式定义业务字段。
- `PacketKind` / `seq` 不暴露为 Lua API，也不借用 CAF request id 建立客户端 pending 表。

详见 [客户端 RPC 路由设计](protocol-routing-design.md)、[网关设计](gateway.md)、[Lua API](lua-api.md)。

---

## AD-06：SessionHandle 边界

**决策**：

- `SessionHandle` 只存在于 network / gateway 边界。
- 登录后每个 `ClientIngress` 必须携带 runtime 注入的可信客户端上下文：Gateway 地址、`session_id`、session epoch、`player_id` 和 protocol profile id。它们是可序列化值，不是连接对象。
- Lua handler 只看到只读 `ClientContext` 与该 RPC 的业务参数；`ClientContext` 可派生用于回包的 `ClientRef`；该值内部保留 protocol profile identity，但不向业务暴露 socket、CAF handle、codec 或 frame。
- Player/Scene/Room/Map Service 通过注册的 server-to-client RPC helper 发送 `ClientEgress`；它们不调用通用 session send API，也不接触 codec / frame / ProtocolPipeline。

**理由**：网络连接所有权归 Gateway，但直接接收客户端 RPC 的任意目标 Service 都必须知道可信玩家身份，并能把响应送回原连接。因此跨 actor 传递稳定的值语义 client reference，而不是连接 handle。这样：

- `SessionHandle` 不会随消息序列化或跨线程传播。
- 目标 Service 无论位于本地还是远端，都能获得可信 `player_id` 和回包引用。
- 业务 Service 与具体协议、socket 实现彻底解耦。
- 断线、重连和 stale 回包的 owner/epoch 校验集中在 Gateway 一处。

---

## 决策与本仓库其他文档的关系

| 决策 | 影响文档 |
|---|---|
| AD-01 | architecture.md、architecture-core-concepts.md、skynet-comparison.md、runtime-messaging.md、roadmap.md |
| AD-02 | runtime-service.md、runtime-player.md、skynet-comparison.md |
| AD-03 | runtime-entity.md、runtime-player.md、architecture.md |
| AD-04 | runtime-player.md |
| AD-05 | gateway.md、protocol-routing-design.md、lua-api.md、runtime-messaging.md |
| AD-06 | runtime-network.md、runtime-player.md、gateway.md、lua-api.md |
