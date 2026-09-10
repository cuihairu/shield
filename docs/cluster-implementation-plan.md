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

### M1 心跳调度 + 诚实状态（无网络，半天）

- bootstrap 启动低频定时线程（`std::jthread` + `heartbeat_interval_ms` 节拍）
  调用 `ClusterManager::tick()`；`stop()` 时收线。
- 效果：transport 缺席时 peer 会经 `online → suspect → offline` 诚实降级，
  `/ops/status` 与 `shield.cluster.nodes()` 不再报告虚假健康，满足
  optional-modules "持续暴露 unhealthy" 契约的前半段。
- 附带修复：`node_epoch` 经 Lua 返回的精度问题（返回 string 或截断为
  53 位安全的派生值，二选一，倾向 string）。
- 测试：`tick()` 降级序列单测（当前 cluster 模块零测试，先补状态机用例）。

### M2 transport：握手 + 心跳（CAF middleman，2~3 天）

- `ClusterTransport` 以 CAF actor 形态实现：
  - 服务端：`middleman().publish(actor, port)` 绑定 `cluster.listen`；
  - 客户端：对每个静态 peer `middleman().remote_actor("shield-cluster", host, port)`，
    断线按 `heartbeat_interval_ms` 退避重连。
- CAF 强类型消息（CAF 自带序列化，不引入 JSON envelope）：

```cpp
struct hello { std::string node_id; uint64_t epoch; uint32_t proto_version; };
struct hello_ack { std::string node_id; uint64_t epoch; };
struct heartbeat { std::string node_id; uint64_t epoch; uint64_t seq; };
struct peer_down { std::string node_id; };
```

- 握手成功：以对端宣告的 `node_id/epoch` 替换 address 占位 → 置 `Online`，
  触发 `on_peer_up`；此后周期 `heartbeat`，停发即由 M1 的 `tick()` 降级。
- 接线：bootstrap `initialize()` 创建 transport 并
  `set_remote_send_fn`（M4 前先注入空实现）。
- 安全注记：CAF 端口无鉴权，Phase 1 以内网隔离为前提；`hello` 预留
  `proto_version` 字段，后续可加 shared secret 校验。
- 测试：同机双节点集成测试（两个 ClusterManager + 两个端口）：
  握手上线、杀对端后 `tick()` 降级、对端恢复重连。

### M3 路由学习（1 天）

- 通告源：`LuaServiceManager` 的 publish/unpublish 状态机
  （`published_names` 变更处）回调 ClusterManager；shutdown 时全量撤销。
- 交换：`hello`/`heartbeat` 捎带本地路由表（首次全量 + 之后增量
  `route_announce{node_id, service_name, service_id}`）。
- 接收：写入既有 `register_route()`；`on_peer_down` 时清除该节点全部
  route cache（防 stale）。
- 效果：`shield.cluster.query()` 从"必然失败"变为语义正确的
  `service_not_found`，且与"节点不可达"错误可区分（M1/M2 后可达性检查可信）。
- 测试：node-a 注册服务 → node-b `query` 命中；node-a 下线 → node-b
  query 返回节点不可达；name 注销 → cache 失效。

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

1. **CAF middleman 接合**：项目对 CAF 的使用偏浅，middleman 的
   publish/remote_actor 与手写 actor 系统的细节（端口占用处理、异步
   remote_actor 解析失败路径）需在 M2 首日打样验证。
2. **call 回包与协程恢复**：跨节点 `envelope_reply` 与既有
   `call_session`/coroutine 机制的对接是最大单项风险；若接合代价过高，
   退路是先交付 send（单向）与 call_timeout（独立临时 session）。
3. **降级可见性**：M1 落地后（M2 之前）所有 peer 将如实显示
   `suspect/offline`——比假 online 诚实，但依赖旧假象的运维看板会先"变红"，
   属预期行为，发布说明需注明。
4. **开放问题**：`cluster.listen` 需要鉴权/加密（Phase 2）；多网卡
   advertise 地址（`hello` 载荷是否携带可达地址）在静态 peers 下暂不需要。
