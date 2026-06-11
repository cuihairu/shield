# 集群运行时语义

本文档包含 Shield 跨进程/跨机器通信（cluster）和旧代码处理相关的运行时语义决策。

`shield_cluster` 是官方可选模块，不属于 `shield_core`，也不是最小运行路径。本文用于提前冻结多节点语义，保证未来实现不会把服务发现、节点编排或 CAF 远程细节泄漏进用户 API。

当前状态：

- 本文 API 和 discovery 策略仍是 optional module 设计稿。
- 本地 `shield.query(name)` 继续只查询本地 registry。
- optional module 的横向 owner、配置归属和 disabled 语义见 [官方可选模块契约](optional-modules.md)。

## shield_cluster 定位

`shield_cluster` 负责跨进程和跨机器通信，不属于 `shield_core`。

边界约束：

- 复用 `ServiceHandle`、`ServiceAddress`、`send/call`、timeout 和错误码语义。
- 不改变本地 `ServiceRegistry` 的规则。
- remote name 只作为 cluster route cache，不进入 core registry。
- 业务 Lua 默认不感知 CAF middleman。
- 节点发现和负载均衡策略不反向成为 core 依赖。

### 与 CAF 的关系

CAF 底层已支持远程 Actor 通信（通过 middleman），`shield_cluster` 在此基础上提供：

| 能力 | CAF 提供 | shield_cluster 补充 |
|------|----------|---------------------|
| 远程 Actor 通信 | ✅ | 封装为 Shield 服务语义 |
| 连接管理 | ✅ | 节点生命周期管理 |
| 消息路由 | ✅ | 服务名路由、路由 cache |
| 节点发现 | ❌ | 可选集成外部系统 |
| 负载均衡 | ❌ | 自定义策略 |
| 心跳状态 | ❌ | online/suspect/offline/removed |

### 实现策略

**基于 CAF middleman：**
- 使用 CAF 的远程 Actor 能力
- 不重新实现网络传输层
- 在 CAF 之上封装 Shield 服务语义

**可选集成外部服务发现：**
- 静态配置（开发环境）
- Kubernetes Service（云原生部署）
- Etcd/Consul/Zookeeper（自建集群）

## Public Surface

`shield_cluster` 只定义两类 public surface：

```lua
local h, err = shield.cluster.query("node-2", "room.public")
local nodes = shield.cluster.nodes()
```

规则：

- `shield.cluster.query(node, name)` 负责远端 name 解析。
- `shield.cluster.nodes()` 返回当前已知节点及状态摘要。
- 普通业务消息仍走统一 `shield.send/call`，不定义 `shield.cluster.send/call`。
- 节点 connect/disconnect、peer 管理属于配置和运维职责，不作为业务 Lua API 暴露。

## Cluster 语义

共同约束：

- `NodeId` 在同一部署内必须唯一。
- handshake 必须携带 `node_id` 和 `node_epoch`。
- 重复 `NodeId` 必须拒绝连接。
- heartbeat 驱动 `online/suspect/offline/removed`。
- remote service handle 带 `{node, epoch, service_id}`。
- remote name 是 cache，不进入 `shield_core` 的本地 registry。

## 节点发现

节点发现属于 `shield_cluster`，不属于 `shield_core`。目标方案可以分层实现：

| 方案 | 适用场景 | 外部依赖 | 配置方式 |
|------|----------|----------|----------|
| **静态配置** | 开发/测试/小型部署 | 无 | `cluster.peers` |
| 广播发现 | 局域网自动发现 | 无 | `cluster.discovery: broadcast` |
| Redis | 小型游戏/已有 Redis | Redis | `cluster.discovery: redis` |
| Kubernetes | 云原生部署 | K8s API | `cluster.discovery: kubernetes` |
| Etcd/Consul | 大型自建集群 | Etcd/Consul | `cluster.discovery: etcd` |

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

### 内置广播发现

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

### Redis 服务发现

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

### 外部服务发现

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

服务发现后的负载均衡策略：

| 策略 | 说明 |
|------|------|
| round-robin | 轮询 |
| least-connections | 最少连接数 |
| consistent-hash | 一致性哈希（按 key） |
| weighted | 加权轮询 |

负载均衡由业务层或 pool service 实现，不进入 core。

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

## 实现状态

`shield_cluster` 目前只是预留模块，尚未实现。完整实现需要：

- 节点发现和连接管理（基于 CAF middleman）。
- 节点心跳和状态管理。
- 跨节点消息路由（服务名 -> 远程 Actor）。
- 远端路由 cache。

**建议最小 cluster 实现范围：**
- 静态配置（零依赖）
- 基本心跳和状态管理
- 远端 route cache
- `send/call` 跨节点错误语义

**后续扩展：**
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

当前已知需要重点清理：

```txt
include/shield/service/service_api.hpp
include/shield/service/service_handle.hpp
src/service/service_api.cpp
src/actor/actor_starter.cpp
```
