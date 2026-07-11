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
- 客户端消息投递到玩家：`shield.send(player_service, "client_message", ...)`，目标仍是 PlayerService 这个 service，而非某个玩家对象。

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

## AD-05：客户端路由一次直达（route_id → handler 闭环）

**决策**：客户端消息路由应当**一次查表直达 handler**，不出现「C++ 路由一次、业务 Lua 再 if/else 判一次」的双重路由。

**完整设计（目标态）**：

1. **注册阶段（启动时建立，双向可查，不丢弃信息）**：
   - 每个业务 handler 注册时建立映射：`route 名 ↔ route_id ↔ handler ↔ target service`。
   - `route_id` 用于运行时高效查表；原始 `route 名` **保留不丢弃**，供日志、调试、错误信息使用——出问题能看到 `4097 = c2s.player.move`，而不是干瞪眼一个数字。
   - 若协议的 route **本身就是整数**，则直接用该 int 作 `route_id`，跳过 hash；若 route 是字符串，才 hash 成 int。两种都支持，不强制 hash。

2. **运行时（一次路由直达）**：
   - C++ 从帧头解出 `route_id` → 查表 → 同时得到「目标 service/进程」和「handler 函数」。
   - 一次查表既决定发给谁、又决定调哪个函数；业务 handler 收到时已经是目标函数，**不写 if/else，不做字符串比较**。

3. **消息形状**：透传到业务的逻辑消息为 `client = { route, payload }`，`route` 是稳定逻辑名，`route_id` 不进业务 Lua。

**codec 与路由无关（附注，非路由依据）**：
- codec 是 **session 级固定**的，不是按 route 选择的：`ProtocolPipeline` 建 pipeline 时绑定 `profile_.default_codec_id`（`src/transport/protocol.cpp:1669,1719`）。
- `codec_for_route()` 忽略 route，恒返回 `default_codec_id` 的 codec；`route.codec_id` 仅作元数据/未来扩展位（`src/transport/protocol.cpp:1510-1517`）。
- 因此不存在「按消息种类切 codec」；解析 `route_id` 的唯一目的是识别消息种类、定位 handler。

**当前实现差距（诚实标注，未闭环）**：

| 设计要求 | 当前代码 | 状态 |
|---|---|---|
| `route_id ↔ route_name` 双向保留 | `RouteTable` 有 | 已做到 |
| `route_id ↔ handler 函数` | 无此映射 | **缺失** |
| `route_id ↔ target service`（分发） | `RouteEntry::target_service` 字段存在，但 bridge 不用它，全投给同一 gateway（`src/lua/lua_gateway_bridge.cpp:26-27`） | **未接上** |
| route 透传到业务 | bridge 丢掉 route，只传 payload（`src/lua/lua_gateway_bridge.cpp:72-88`） | **缺失** |
| 一次路由直达 handler | 业务被迫在 `on_client_message` 内自己 if/else 二次判断 | **未做到** |

结论：当前路由链断在中途（注册表只建了 `id↔name` 前半截，`id↔handler`/`id↔target`/自动 dispatch 后半截未接）。补闭环是 roadmap 项，见 [客户端交互契约收敛](roadmap.md)。

**不做**：

- 不引入框架级 client RPC（不做 `client.call`、seq pending map、自动 response 关联）。
- 客户端 request/response/push 的 correlation token（request id 等）由业务 payload schema 字段自管。
- `PacketKind` / `seq` 不暴露为 Lua client API，不建立框架级请求-响应关联。

详见 [网关设计](gateway.md)、[协议路由设计](protocol-routing-design.md)、[Lua API](lua-api.md)。

---

## AD-06：SessionHandle 边界

**决策**：

- `SessionHandle` 只存在于 network / gateway 边界。
- 跨 service payload 只传 `session_id`（标量）或未来 `PlayerRef`，**不**传 `SessionHandle`，也**不**传完整玩家会话对象。
- PlayerService 不调用 `session:send`、不接触 codec / frame / ProtocolPipeline。

**理由**：网络连接所有权归 gateway；PlayerService 只持有 `session_id` 作为标量身份，业务出站消息通过普通 service 消息回到 gateway，由 gateway 统一编码写回。这样：

- SessionHandle 不会随消息序列化或跨线程传播。
- PlayerService 与具体协议、socket 实现彻底解耦。
- 断线/重连/stale 消息的 owner 校验集中在 gateway 一处。

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
