# 集群运行时语义

本文档包含 Shield 跨进程/跨机器通信（cluster）和旧代码处理相关的运行时语义决策。

`shield_cluster` 是官方可选模块，不属于 `shield_core`，也不是最小运行路径。本文用于提前冻结多节点语义，保证未来实现不会把服务发现、节点编排或 CAF 远程细节泄漏进用户 API。

当前状态：

- 本文冻结 `shield_cluster` optional module 的第一版契约。
- 本地 `shield.query(name)` 继续只查询本地 registry。
- optional module 的横向 owner、配置归属和 disabled 语义见 [官方可选模块契约](optional-modules.md)。

实现状态（2026-09，M1 心跳调度 + M2 CAF middleman transport 已落地，与 [Phase 1 实现范围](#phase-1-实现范围)一致）：

- **已实现（跨节点握手/保活全链路）**：`cluster.*` 配置解析；节点状态数据结构与快照查询；`shield.cluster.nodes()/node_id()/node_epoch()`；`/ops/status` cluster 块与 console 命令；bootstrap 生命周期接线；**心跳调度线程**（`start()` 起内部 `std::jthread` 按 `heartbeat_interval_ms` 驱动 `tick()` 降级，`stop()` 收线）；**CAF middleman transport**（M2）：绑定 `cluster.listen`、主动拨号全部静态 peers、hello/hello_ack 握手（协议版本 v1，身份采纳只在拨号侧完成）、按心跳节拍互发 `HeartbeatMsg`、连接断开即刻 offline 并清路由、带 2s 超时的异步重拨（对端重启可自愈，新 epoch 使旧路由失效）。双节点集成测试以两个真实 `caf::actor_system` 走 BASP 验证握手/保活/下线/重启重连（`tests/cluster/test_cluster_transport.cpp`）。
- **未实现（数据面缺失）**：远端 route 学习（`register_route` 无调用方，route cache 只会被握手/下线清除）；跨节点投递（`set_remote_send_fn` 无注入方，`send_remote` 恒返回 false）。
- **当前实际行为**：静态 peers 以 `connecting` 占位（临时以拨号地址为节点标识）；握手完成后改名为对端宣布的 `node_id` 并转为 `online`；此后心跳保活，超时按 `online → suspect → offline` 如实降级。对端进程退出会经连接断开事件直接置 `offline`（无 suspect 宽限）并清除其路由缓存；对端重启后自动重连，新 epoch 触发路由失效。`cluster.listen` 绑定失败视为致命错误，启动即失败。`shield.cluster.query()` 因 route cache 为空而必然返回 `service_not_found`（M3 接入 route 学习前如此）。后续实现路线见 [cluster 实现方案](cluster-implementation-plan.md)。

## shield_cluster 定位

`shield_cluster` 负责跨进程和跨机器通信，不属于 `shield_core`。

边界约束：

- 复用 `ServiceHandle`、`ServiceAddress`、`send/call`、timeout 和错误码语义。
- 不改变本地 `ServiceRegistry` 的规则。
- remote name 只作为 cluster route cache，不进入 core registry。
- 业务 Lua 默认不感知 CAF middleman。
- 节点发现和负载均衡策略不反向成为 core 依赖。

### 与 CAF 的关系

CAF 底层已支持远程 Actor 通信（通过 middleman），`shield_cluster` 在此基础上封装 Shield 服务语义：

| 能力 | CAF 提供 | shield_cluster 补充 |
|------|----------|---------------------|
| 远程 Actor 通信 | ✅ | 封装为 Shield 服务语义 |
| 连接管理 | ✅ | 节点生命周期管理 |
| 消息路由 | ✅ | 服务名路由、路由 cache |
| 节点发现 | ❌ | Phase 1 仅支持静态配置 |
| 负载均衡 | ❌ | 不进入 Phase 1 |
| 心跳状态 | ❌ | online/suspect/offline/removed 状态模型已有；降级循环由 M1 心跳调度线程驱动，心跳交换由 M2 transport 承担（握手后按节拍互发 HeartbeatMsg，可把 suspect/offline 拉回 online） |

### Phase 1 实现策略

**基于 CAF middleman：**
- 使用 CAF 的远程 Actor 能力
- 不重新实现网络传输层
- 在 CAF 之上封装 Shield 服务语义

**静态配置：**
- Phase 1 只支持静态 peers。
- 不集成 Kubernetes、Etcd、Consul、Zookeeper 或广播发现。
- 不提供透明负载均衡、自动 sharding 或 leader election。

## Public Surface

`shield_cluster` 只定义两类 public surface：

```lua
local h, err = shield.cluster.query("node-2", "room.public")
local nodes = shield.cluster.nodes()
```

规则：

- `shield.cluster.query(node, name)` 从 cluster route cache 读取远端 name 解析结果；cache 未命中时返回明确错误，不伪造成功。
- `shield.cluster.nodes()` 返回当前已知节点及状态摘要。
- 普通业务消息仍走统一 `shield.send/call`，不定义 `shield.cluster.send/call`。
- 节点 connect/disconnect、peer 管理属于配置和运维职责，不作为业务 Lua API 暴露。
- Phase 1 只支持显式 `(node_id, service_name)` 寻址。

## Cross-Node Main Path

跨服默认主路径是“业务消息路由”，不是“客户端协议帧转发”。

```text
Lua service
  -> shield.send / shield.call
  -> shield_core
  -> shield_cluster
  -> CAF remote transport
  -> remote shield_core
  -> remote service
```

规则：

- 跨服业务交互默认走 `shield.send/call`。
- 本地和远端尽量复用同一套 service/message/timeout/error 语义。
- 业务代码面向逻辑 service 名称或 `ServiceHandle`，不直接面向 CAF actor 或网络连接。
- `shield_cluster` 负责把本地 service 语义扩展到远端，不再复制一套 cluster 专用业务调用模型。

推荐目标：

- 点对点业务请求：`shield.send/call`
- 玩家归属路由：发给 owning service 或 `PlayerRef`
- 全局广播、共享状态、持久化队列：走 `shield_global` 或数据插件

不推荐作为默认跨服主路径的做法：

- 直接暴露 CAF API 给业务层
- 让业务层长期持有和转发客户端 raw frame
- 跨 service 或跨节点传递 `SessionHandle`

## Raw Forwarding As Exception Path

`ForwardRaw` 只适合作为协议数据面的例外路径，而不是常规跨服模式。

适用场景：

- 代理/边缘网关需要把某些 header-route frame 原样转交给后端专用网关
- 协议迁移期暂时保留旧二进制帧
- 某些链路明确要求中间节点不解 body

约束：

- `ForwardRaw` 应终止在 C++ forwarding path，不应成为通用 Lua 业务 API。
- 它解决的是 transport/protocol 转发，不是业务服务协作。
- 一旦进入后端业务层，推荐尽快回到 `shield.send/call` 的逻辑消息模型。

## Cluster 语义

共同约束：

- `NodeId` 在同一部署内必须唯一。
- handshake 必须携带 `node_id` 和 `node_epoch`。
- 重复 `NodeId` 必须拒绝连接。
- heartbeat 驱动 `online/suspect/offline/removed`。
- remote service handle 带 `{node, epoch, service_id}`。
- remote name 是 cache，不进入 `shield_core` 的本地 registry。

## 节点发现

节点发现属于 `shield_cluster`，不属于 `shield_core`。Phase 1 只实现静态配置：

| 方案 | 适用场景 | 外部依赖 | 配置方式 |
|------|----------|----------|----------|
| **静态配置** | 开发/测试/小型部署 | 无 | `cluster.peers` |
| 广播发现 | Phase 2+ | 无 | `cluster.discovery: broadcast` |
| Redis | Phase 2+ | Redis | `cluster.discovery: redis` |
| Kubernetes | Phase 2+ | K8s API | `cluster.discovery: kubernetes` |
| Etcd/Consul | Phase 2+ | Etcd/Consul | `cluster.discovery: etcd` |

### 默认实现：静态配置

零依赖，适合开发、测试和小型部署（< 10 节点）：

```yaml
cluster:
  node_id: "node-1"
  listen: "0.0.0.0:9000"
  peers:
    - "node-2:9000"
    - "node-3:9000"
```

### 广播发现（Phase 2+）

局域网自动发现，无需配置 peers 列表：

```yaml
cluster:
  node_id: "node-1"
  listen: "0.0.0.0:9000"
  discovery:
    type: broadcast
    broadcast_port: 9001    # 广播端口
    interval: 5000          # 广播间隔（ms）
```

工作原理：
- 节点启动后定期广播自己的地址
- 收到广播的节点自动建立连接
- 节点离开时通过心跳超时检测

### Redis 服务发现（Phase 2+）

基于 Redis 的服务发现，适合小型游戏和已有 Redis 的项目。成本低、实现简单、可靠性足够。

```yaml
cluster:
  node_id: "node-1"
  listen: "0.0.0.0:9000"
  discovery:
    type: redis
    redis:
      host: "localhost"
      port: 6379
      password: ""
      db: 0
      prefix: "shield:nodes"    # Redis key 前缀
      ttl: 10                   # 节点注册 TTL（秒）
      heartbeat_interval: 3000  # 心跳间隔（ms）
```

**工作原理：**

```
┌─────────────────────────────────────────────────────────┐
│  1. 节点启动                                             │
│     - 连接 Redis                                        │
│     - 注册节点信息到 Redis                                │
│     - 设置 TTL（默认 10 秒）                              │
├─────────────────────────────────────────────────────────┤
│  2. 心跳续期                                             │
│     - 每 3 秒续期一次                                    │
│     - 更新节点时间戳                                     │
├─────────────────────────────────────────────────────────┤
│  3. 发现其他节点                                         │
│     - 定期扫描 Redis 中的节点列表                         │
│     - 建立连接                                           │
├─────────────────────────────────────────────────────────┤
│  4. 节点下线                                             │
│     - 正常关闭：主动删除 Redis key                        │
│     - 异常崩溃：TTL 过期自动删除                           │
└─────────────────────────────────────────────────────────┘
```

**Redis 数据结构：**

```
# 节点注册（Hash）
HSET shield:nodes:node-1
  addr "192.168.1.100:9000"
  status "online"
  started_at "1234567890"
  last_heartbeat "1234567893"

# 节点列表（Set）
SADD shield:nodes "node-1" "node-2" "node-3"

# 节点计数
GET shield:nodes:count  # "3"
```

**优点：**
- 大多数项目已有 Redis，无需额外部署
- 实现简单，可靠性足够
- 成本低，运维简单
- 支持多机房部署（Redis 复制）

**适用场景：**
- 小型游戏（< 50 节点）
- 已有 Redis 基础设施
- 不想引入 Etcd/Consul 等重型中间件

### 外部服务发现（Phase 2+）

大型部署可选集成外部系统：

```yaml
# Kubernetes
cluster:
  node_id: "node-1"
  listen: "0.0.0.0:9000"
  discovery:
    type: kubernetes
    namespace: "game"
    service_name: "shield-cluster"

# Etcd
cluster:
  node_id: "node-1"
  listen: "0.0.0.0:9000"
  discovery:
    type: etcd
    endpoints:
      - "http://localhost:2379"
    prefix: "/shield/nodes"
```

## 负载均衡

负载均衡不进入 Phase 1。后续可选策略：

| 策略 | 说明 |
|------|------|
| round-robin | 轮询 |
| least-connections | 最少连接数 |
| consistent-hash | 一致性哈希（按 key） |
| weighted | 加权轮询 |

Phase 1 中，负载均衡由业务层或 pool service 自行实现，不进入 core 或 `shield_cluster` public API。

## 单机多进程模式

对于需要多进程但不需要分布式的场景，支持单机多进程部署：

```yaml
# node-1.yaml
cluster:
  node_id: "node-1"
  listen: "127.0.0.1:9000"
  peers:
    - "127.0.0.1:9001"

# node-2.yaml
cluster:
  node_id: "node-2"
  listen: "127.0.0.1:9001"
  peers:
    - "127.0.0.1:9000"
```

启动命令：

```bash
./shield --config node-1.yaml &
./shield --config node-2.yaml &
```

适用场景：
- 利用多核 CPU
- 故障隔离（一个进程崩溃不影响其他）
- 热更新（逐个进程重启）

## Phase 1 实现范围

`shield_cluster` 第一版实现范围：

- 静态 peers 配置。
- 静态 peers 配置解析。
- 节点状态快照和 `online/suspect/offline/removed` 状态模型。
- 远端路由 cache 结构。
- 显式 `(node_id, service_name)` route cache 查询。

当前源码实现状态（`src/cluster/`，2026-09 实测核对）：

**已实现，行为可信：**

- `cluster.node_id/listen/heartbeat_interval_ms/suspect_timeout_ms/offline_timeout_ms/peers` 配置解析。
- 节点状态数据结构、`nodes()` / `find_node()` / `check_node_reachable()` 查询。
- `shield.cluster.nodes()/node_id()/node_epoch()` Lua 查询（`node_id`、`node_epoch` 返回真实本地元数据；`node_epoch` 与节点快照 `epoch` 均为十进制字符串）。
- `/ops/status` 的 cluster 快照、console `root.*` 命令、bootstrap 创建/停止接线。
- **心跳调度线程（M1）**：`start()` 启动内部 `std::jthread`，按 `heartbeat_interval_ms`（下限 50ms）驱动 `tick()` 降级；`stop()` 先收线再清理节点状态。
- **CAF middleman transport（M2，`cluster_transport.cpp`）**：绑定 `cluster.listen`（`0.0.0.0`/`*` 绑全部接口）；按心跳节拍带 2s 超时异步拨号全部静态 peers，断线自动重拨；hello/hello_ack 握手（协议版本 v1，版本不符拒绝完成握手）。身份采纳只发生在拨号侧：ack 的 sender 即本节点拨号所得 proxy，与 peer 条目一一对应，不依赖对端播报的监听地址字符串。握手采纳 `node_id` + `epoch`，`connecting` 占位条目就此改名；epoch 变化的再握手（对端重启）清除该节点路由缓存。心跳按节拍互发，`on_heartbeat` 刷新 liveness 并可把 `suspect`/`offline` 拉回 `online`；连接断开事件直接置 `offline` 并清路由。
- bootstrap 在 CAF actor system 构建后启动 transport（集群 wire 类型必须在任何 `actor_system` 构造前注册，已与 `initialize_caf_types()` 并列处理）；`cluster.listen` 绑定失败为致命错误；两条 teardown 路径均先停 transport 再停 manager。
- `tests/cluster/test_cluster_manager.cpp`（15 例：状态机、握手采纳、epoch 失效、心跳恢复、下线清路由、握手超时降级）与 `tests/cluster/test_cluster_transport.cpp`（2 例：双 `caf::actor_system` 真实 BASP 握手/保活/下线/同端口重启重连）。

**未实现（数据面缺失，M3/M4）：**

- **route 学习**：`register_route()` 无调用方；route cache 只会被握手（epoch 变化）、下线事件清除，永远不会命中。
- **投递**：`set_remote_send_fn()` 无注入方，`send_remote()` 恒返回 `false`；`RemoteSendFn` 的注入点是预留的 transport 接缝（单测已覆盖注入行为）。

**由此产生的当前实际行为（业务与运维须知）：**

- 静态 peers 以 `connecting` 占位（临时以拨号地址为节点标识）；握手完成转为 `online` 并按真实 `node_id` 可查。对端不可达时，`connecting` 在 `offline_timeout_ms` 后如实降级为 `offline`——配置了不存在的 peer 不会被报成健康节点。
- 在线 peers 由心跳保活；对端进程退出即刻 `offline` 并清其路由（无 suspect 宽限）；对端重启后自动重连，新 epoch 触发路由失效。`suspect_timeout_ms` 后降级 `suspect`，`offline_timeout_ms` 后降级 `offline`，由心跳调度线程自动完成。
- `shield.cluster.query(node, name)` 在 route cache 为空时返回 `service_not_found`；peer 降级后同一调用先被可达性检查以 `node_suspect`/`node_offline` 拦下，"节点不可达"与"路由未注册"自此可区分。
- `node_epoch` 与节点快照 `epoch` 以十进制字符串经 Lua/JSON 暴露（uint64 直接过 double 会丢精度）；stale-handle 比对须按字符串比较。M2 起握手 epoch 具备跨节点语义：peer 重启（新 epoch）即宣告其旧路由全部失效。

因此：启用 `cluster.node_id` 后，集群会呈现真实的连接/握手/保活/下线视图（健康节点 `online`，故障节点 `offline`）；但**跨节点数据面尚未打通**，`shield.cluster.query` 必然 `service_not_found`、跨节点投递尚未实现。未配置 `cluster.node_id` 时整条路径不激活，单节点部署不受影响。远端连接失败的 degrade 契约（见 [官方可选模块契约](optional-modules.md) 的"必须持续暴露 unhealthy 状态"）已满足：故障节点如实暴露 `offline`。

**Phase 1 不做：**
- 动态服务发现。
- 透明负载均衡。
- 全局唯一服务名竞争。
- 分布式一致性、leader election、自动 sharding。
- 玩家迁移、全局锁、排行榜、跨节点配置推送。

**后续扩展：**
- Redis discovery。
- Kubernetes 集成
- Etcd/Consul 集成
- 高级路由策略

## 旧代码处理策略

旧模块处理顺序：

1. 标记所有 public header 中的 CAF 泄漏点。
2. 建立 forbidden include 检查。
3. 抽出 `shield_base` 基础类型。
4. 重建 `ServiceHandle` / `ServiceRegistry`。
5. 替换旧 `service_api` 中 CAF 直出 API。
6. 把 discovery/metrics/health/plugin/DI 等旧模块移出 core 路径。
7. 保留有价值代码时必须归入明确 target。
8. 无 target 归属的旧代码删除或移入实验区。

历史清理项（以下文件已在重构中删除，此处仅存目）：

```txt
include/shield/service/service_api.hpp
include/shield/service/service_handle.hpp
src/service/service_api.cpp
src/actor/actor_starter.cpp
```
