# shield_cluster 实现方案

Status: draft implementation plan（未评审、未排期）。现状核对与实测行为见
[集群运行时语义](runtime-cluster.md) 的"当前状态"与"Phase 1 实现范围"；
本方案不改变已冻结的 public surface 契约。

## 1. 目标

把 `shield_cluster` 从"本地状态机骨架"补齐为 Phase 1 契约定义的最小可用跨节点运行时：

- peer 连接与握手（transport 落地，`cluster.listen` 真正绑定）
- 心跳驱动的真实节点状态（消灭"假 online"，满足 optional-modules 的
  unhealthy 暴露契约）
- 远端路由学习（`shield.cluster.query` 从必然失败变为 cache 未命中才失败）
- 跨节点 `send` / `call` 投递（业务 Lua 经统一 `shield.send/call` 透明寻址）

Phase 1 契约红线（实现不得越过）：静态 peers、无发现、无负载均衡；
remote name 不进 core registry；业务 Lua 不感知 CAF middleman；
本地 `send/call/query` 行为完全不变。

## 2. 现状与差距

| 能力 | 现状 | 差距 |
| --- | --- | --- |
| 状态机数据结构 | ✅ `cluster_manager.cpp` | 只缺驱动方 |
| 配置解析 | ✅ `parse_cluster_config()` | 无 |
| transport | ❌ 不连接、不监听 | 全部 |
| 心跳/降级 | ❌ `tick()` 无调用方 | 调度循环 + 心跳来源 |
| 路由学习 | ❌ `register_route()` 无调用方 | 本地通告 + peer 交换 |
| 投递 | ❌ `set_remote_send_fn()` 无注入方 | envelope + 远端派发 + call 回包 |
| Lua 寻址 | ❌ `shield.send/call` 无远程分支；`parse_remote_target()` 零调用方 | send/call 入口接线 |

可复用的既有接缝（设计已留好，本方案只是填充）：
`RemoteSendFn` 注入点、`register_route()` cache、`parse_remote_target()`
寻址解析、`tick()` 降级状态机、bootstrap 已加载的 CAF middleman。

## 3. 总体架构

```text
Lua: shield.send / shield.call("node-b:room", ...)
  -> lua_api: parse_remote_target() 命中 "node:service" 形态
     -> ClusterManager::send_remote()            （RemoteSendFn 注入）
        -> ClusterTransport: CAF middleman envelope
           -> peer 的 ClusterTransport actor
              -> 本地 registry 解析 service_id
              -> shield_core dispatch（复用既有派发路径）
           <- reply envelope（call）
        <- 结果回填到发起方的 call_session / 协程 resume
```

新增组件只有一个：`ClusterTransport`（拟 `src/cluster/cluster_transport.cpp`），
负责 CAF middleman publish/connect 与四类控制消息；`ClusterManager` 保持
纯状态机职责不变，通过两个接缝与 transport 解耦：

- `RemoteSendFn`（已有）：transport 注入数据面。
- 新增 `PeerListener` 回调（`on_peer_up(node_id, epoch)` /
  `on_peer_down(node_id)`）：transport 通知控制面事件，驱动路由失效。

## 4. 里程碑

每个里程碑独立可合入、独立有价值、不破坏单节点路径。

### M1 心跳调度 + 诚实状态（无网络，半天）✅ 已落地（2026-09）

- bootstrap 启动低频定时线程（`std::jthread` + `heartbeat_interval_ms` 节拍）
  调用 `ClusterManager::tick()`；`stop()` 时收线。
  实现落点：线程内聚在 `ClusterManager::start()/stop()` 内部（bootstrap 两条
  收线路径都只需调 `stop()`，无需各自管理线程）。
- 效果：transport 缺席时 peer 会经 `online → suspect → offline` 诚实降级，
  `/ops/status` 与 `shield.cluster.nodes()` 不再报告虚假健康，满足
  optional-modules "持续暴露 unhealthy" 契约的前半段。
- 附带修复：`node_epoch` 经 Lua 返回的精度问题（返回 string 或截断为
  53 位安全的派生值，二选一，倾向 string）。**实际落地**：Lua 绑定与
  JSON 序列化（/ops/status、console）统一改为十进制字符串。
- 测试：`tests/cluster/test_cluster_manager.cpp`（状态机降级序列、心跳线程
  自驱动、route cache、`parse_remote_target`、注入式投递接缝）；CI 新增
  `SHIELD_ENABLE_CLUSTER=ON` 的 Cluster job（`ctest -L cluster`）。

### M2 transport：握手 + 心跳（CAF middleman，2~3 天）✅ 已落地（2026-09）

- `ClusterTransport` 以 CAF actor 形态实现：
  - 服务端：`middleman().publish(actor, port)` 绑定 `cluster.listen`；
  - 客户端：对每个静态 peer 经 middleman actor 异步拨号（`connect_atom`，
    2s 超时；**不用** `remote_actor()`——其内部 `infinite` 超时会卡死重连循环，
    对端重启窗口即触发），断线按 `heartbeat_interval_ms` 重试。
- CAF 强类型消息（CAF 自带序列化，不引入 JSON envelope；`inspect()`
  手写——`CAF_ADD_TYPE_ID` 只注册类型身份，不生成序列化）：

```cpp
struct hello { std::string node_id; uint64_t epoch; uint32_t proto_version; };
struct hello_ack { std::string node_id; uint64_t epoch; uint32_t proto_version; };
struct heartbeat { std::string node_id; uint64_t epoch; uint64_t seq; };
```

- 握手成功：以对端宣告的 `node_id/epoch` 替换 address 占位 → 置 `Online`。
  身份采纳只在拨号侧（ack sender == 拨号所得 proxy，与 peer 条目一一对应，
  不解析对端播报地址）；两侧互拨因此互相采纳。`on_peer_up` 未单设，由
  `on_handshake`/`on_heartbeat` 承担。
- 接线：bootstrap 在 actor system 构建后创建/启动 transport；listen 失败
  即初始化失败。注意集群 wire 类型必须在任何 `actor_system` 构造前注册
  （`init_cluster_caf_types()`，与 `initialize_caf_types()` 并列）。
- 安全注记：CAF 端口无鉴权，Phase 1 以内网隔离为前提；`hello` 预留
  `proto_version` 字段，后续可加 shared secret 校验。
- 测试：`test_cluster_manager.cpp` 扩至 15 例（握手采纳、epoch 失效、
  心跳恢复、下线清路由、握手超时降级）；`test_cluster_transport.cpp`
  双 `caf::actor_system` 真实 BASP 集成测试（握手、心跳保活、杀对端
  即 offline、同端口重启重连且新 epoch 清路由）。

### M3 路由学习（1 天）✅ 已落地（2026-09）

- 通告源 ✅：`LuaServiceManager::set_name_change_notifier(fn)` 观察每一次
  已提交的发布变更（spawn 发布、`shield.register`/`unregister`、on_init
  失败回滚、服务退出收回；空 `service_id` = 收回），在 registry 锁外回调；
  bootstrap 注入到 `ClusterManager::on_local_route_changed()`。
  `lua_service.cpp` 零 cluster 依赖（无 ifdef）。
- 交换 ✅（与原计划偏离，实测更稳）：放弃增量 `route_announce`，改为
  **完整表随每个心跳捎带**（`RoutesMsg`）+ **握手完成后立即补发一次**。
  理由：幂等（丢包/乱序自愈）、无需撤销协议（空表自然传播"服务已全部
  下线"）、实现面小（Phase 1 服务名量级小，全量表成本可忽略）。
- 接收 ✅：`on_routes()` 要求节点已采纳且 epoch 匹配（stale 实例的表
  静默丢弃），整桶替换；`on_peer_down`/新 epoch 再握手继续清整桶。
- 效果 ✅：`shield.cluster.query()` 从"必然失败"变为可命中远端真实发布
  （收敛延迟：握手后 ≤ 一个心跳周期），且与"节点不可达"错误可区分。
  注意：只完成名字解析，投递仍是 M4。
- 测试 ✅：manager 单测扩至 18 例（本地路由表发布/收回快照、`on_routes`
  整桶替换 + epoch 校验 + 未知节点丢弃 + 空 service_id 过滤、占位键早期
  路由不泄漏进采纳身份）；transport 集成测试扩至 3 例（握手前发布 →
  握手后即命中、运行中发布/收回随心跳传播、对端断连 → offline + 路由
  清除）；`test_cov_lua_service2` 补 2 例钩子生命周期（spawn/register/
  unregister/exit 事件序列、on_init 失败回滚只收回 on_init 发布的名字）。

### M4 跨节点 send/call 投递（2~4 天，最大项）

- `RemoteSendFn` 实装：查 route cache 得 `service_id` → envelope 发往目标
  节点 transport actor：

```cpp
struct envelope {
  std::string service_id;   // 目标 service（远端视角的本地 id）
  std::string method;
  std::string args_json;    // 复用既有 JSON 参数编码
  uint64_t call_session;    // call 回包关联；send 为 0
  std::string source_node;  // 回包路由
};
struct envelope_reply { uint64_t call_session; std::string result_json; std::string error_code; };
```

- 目标节点：envelope → 本地 `send/call` 派发路径（复用 shield_core 语义、
  错误码、timeout），call 的结果经 `envelope_reply` 原路返回。
- Lua 接线（唯一改动 `lua_api.cpp` 的点）：`shield.send/call` 入口先
  `parse_remote_target()`；命中且目标非本节点 → `send_remote`；
  `call` 接入既有 coroutine-aware pending-call 机制（`call_session` 关联
  `envelope_reply`，超时语义与本地一致）。
- 不可达语义：peer 非 Online → 立即失败，错误码复用 `node_offline` /
  `node_suspect`（`check_node_reachable()` 已有），retryable 标记降级路径
  与本地 `service_not_found` 区分。
- 测试：双节点端到端（node-a call node-b echo service；node-b 停机后
  call 立即失败且错误码正确；call 超时与本地同形）。

### M5 观测收尾（半天）

- `/ops/status` cluster 块补充：连接数、重连计数、收发消息计数、最近
  一次心跳时延。
- console `root.cluster` 命令同步；`runtime-cluster.md` 把"未实现"清单
  翻转为已实现，并撤销 optional-modules.md 的实现状态注记。

## 5. 关键设计决策

| 决策 | 选择 | 理由 |
| --- | --- | --- |
| 传输层 | CAF middleman（契约已定） | 不重新实现传输层；项目已 load middleman |
| 消息格式 | CAF 强类型消息 | 自带序列化与版本兼容；业务 JSON 只出现在 envelope 载荷内 |
| 心跳驱动 | transport 周期发、`tick()` 定时降级 | 状态机仍是唯一裁决者，transport 只喂心跳时间戳 |
| call 语义 | envelope + `call_session` 关联既有协程 pending-call | 与本地 call 同一套超时/协程恢复路径，不另造 RPC 栈 |
| 状态裁决 | peer state 以本节点视角为准，不传播状态 | 避免 gossip 复杂度；Phase 2 的发现机制再议 |
| 零开销 | 未启用 `cluster.node_id` 时零线程零端口；`SHIELD_ENABLE_CLUSTER=OFF` 零编译 | 与现状一致，单节点路径不受影响 |

## 6. 测试策略

- **单测（新增 `tests/cluster/`）**：状态机降级序列、route cache 失效、
  `parse_remote_target` 边界（现有实现已可测，M1 前先补）。
- **集成（双节点，同机双端口，可跨平台）**：握手/心跳/断线重连/路由交换/
  端到端 send/call/错误码矩阵。
- **回归**：未启用 cluster 的全量既有测试必须零变化。
- **CI**：cluster 测试纳入主 workflow（纯 TCP，无需 POSIX_ONLY 豁免）。

## 7. 风险与开放问题

1. **CAF middleman 接合** ✅ M2 打样已验证：publish/`connect_atom` 异步拨号
   可行；踩坑实录——`remote_actor()` 内部 `infinite` 超时会卡死重连循环
   （已改带超时的异步拨号）；`anon_send` 不携带 sender，hello/ack 必须用
   `self->send` 才能让对端回包；集群 wire 类型必须在 `actor_system` 构造前
   注册，否则 CAF 直接 CAF_CRITICAL。
2. **call 回包与协程恢复**：跨节点 `envelope_reply` 与既有
   `call_session`/coroutine 机制的对接是最大单项风险；若接合代价过高，
   退路是先交付 send（单向）与 call_timeout（独立临时 session）。
3. **降级可见性** ✅ M2 后失效：peer 视图即真实连接视图（握手成功才
   `online`），不存在假象期；看板语义与真实状态一致。
4. **开放问题**：`cluster.listen` 需要鉴权/加密（Phase 2）；多网卡
   advertise 地址（`hello` 载荷是否携带可达地址）在静态 peers 下暂不需要。
