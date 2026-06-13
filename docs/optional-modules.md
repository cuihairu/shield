# 官方可选模块契约

本文冻结 Shield 官方可选模块的**横向架构契约**：哪些能力属于 optional module、它们各自拥有哪一层 public surface、配置段和错误域由谁拥有、模块之间允许如何协作。

本文不替代各模块自己的专题文档；它的作用是防止 `shield_cluster`、`shield_global`、`shield_player`、`shield_server`、`shield_ops` 在后续实现中重新侵入 `shield_core` 或互相越界。

## 定位

官方可选模块满足四个条件：

- 不属于 `shield_core` 最小语义。
- 可以完全关闭，不影响最小部署路径。
- 依赖已公开的 core/runtime contract，而不是反向修改它们。
- 有独立的 public surface、配置段、错误归属和测试矩阵。

反过来说，凡是必须常驻最小运行路径、会改写 `ServiceHandle` / `send/call` / `SessionHandle` / core config 规则的能力，都不应该放进 optional module。

## 通用规则

### 1. 显式启用

optional module 只有在以下条件同时满足时才可用：

- 二进制已包含该模块。
- bootstrap 显式注册该模块。
- 配置显式启用该模块或提供该模块要求的配置段。

未启用时：

- 不能 silently no-op。
- 不能让 core 误以为该模块已存在。
- 如果 runtime 暴露了该模块的入口，调用失败必须返回 `module_unavailable` 或同等级明确错误。
- 如果配置文件包含该模块配置段，但二进制未构建或 bootstrap 未启用该模块，启动必须失败并输出明确模块名和配置路径。

### 2. 不改写 core 契约

optional module 可以扩展以下内容：

- 新命名空间
- 新配置段
- 新只读 snapshot
- 新错误域
- 新管理端点

optional module 不可以改写以下内容：

- `ServiceHandle` 身份规则
- 本地 `ServiceRegistry` 语义
- `shield.send/call/spawn/exit` 基础语义
- `shield.query(name)` 的“本地 registry only”约束
- `SessionHandle` 不可跨 service 传递的规则
- core config schema 的 owner

### 3. 一模块一 owner

每个 optional module 都必须自己拥有：

- public surface
- 配置段
- 错误归属
- 验收矩阵

不能把这些责任留给“之后再看”或散落在多个 runtime 文档里。

### 4. 初始化失败策略

optional module 初始化失败默认 fail fast。例外必须由模块自己的运行时文档显式说明，并且不能改变 core 成功启动语义。

当前冻结策略：

| 模块 | 配置段存在但模块未启用 | 显式启用后初始化失败 | 可 degrade 的场景 |
| --- | --- | --- | --- |
| `shield_cluster` | 启动失败 | 默认 fail fast | 远端连接失败可退化到单节点，但必须持续暴露 unhealthy 状态 |
| `shield_global` | 启动失败 | fail fast | 无 |
| `shield_player` | 启动失败 | fail fast | 无 |
| `shield_server` | 启动失败 | fail fast | 无 |
| `shield_ops` | 启动失败 | fail fast；管理端口绑定失败也 fail fast | metrics exporter 后端短暂失败可保持 unhealthy |

### 5. 只沿公开依赖方向扩展

```text
shield_core
  -> first-party runtime modules
  -> optional modules
```

禁止反向依赖：

- `shield_core` 不能依赖任何 optional module。
- runtime module 不能为了 optional module 改写 core 语义。
- `shield_ops` 只能读 snapshot，不能成为其他模块的强依赖。

## 模块总表

| 模块 | 最终职责 | public surface | owner config | 主要依赖 | disabled 时要求 |
| --- | --- | --- | --- | --- | --- |
| `shield_cluster` | 多节点地址语义、远端路由 cache、节点心跳、可选发现 | `shield.cluster.*` + remote `ServiceHandle` routing | `cluster` | `shield_core`，必要 runtime snapshot | 本地 `send/call/query` 行为完全不变 |
| `shield_global` | 跨进程共享数据、分布式锁、排行榜、队列、限流器、调度 | `shield.global()`、`shield.mutex()`、`shield.rank()` 等 | `global` | `shield_data`，可选 `shield_cluster` | raw `shield.db.*` / `shield.redis.*` 仍独立可用 |
| `shield_player` | 认证、重连、离线消息、PlayerSession、PlayerManager | `shield.player.*`、`PlayerSession`、player hooks | `player`、`player_manager` | `shield_net`、`shield_lua`、`shield_data`，可选 `shield_global` | gateway 回调和普通 service 语义仍可独立工作 |
| `shield_server` | 服务器状态、维护模式、关闭广播、运行时信息 | `shield.server()`、`server_manager` service | `server_manager` | `shield_core`，可读 `shield_player` / `shield_global` 状态 | bootstrap 和 service 运行路径不依赖它 |
| `shield_ops` | metrics、health、console、profile、diagnostics | HTTP/admin endpoints；不提供业务 Lua API | `ops` | runtime snapshot、module status | runtime 正常运行不依赖任何 ops 入口 |

## 跨模块协作矩阵

| from | to | 允许 | 不允许 |
| --- | --- | --- | --- |
| `shield_cluster` | `shield_core` | 复用 handle、timeout、错误语义 | 改写本地 registry、把发现逻辑塞进 core |
| `shield_global` | `shield_data` | 复用 raw Redis/DB 能力 | 直接拥有连接池或底层驱动 |
| `shield_player` | `shield_net` | 消费 `session_id`、gateway 回调、session 状态 | 跨 service 传 `SessionHandle` |
| `shield_server` | `shield_player` / `shield_global` | 读取状态、广播维护流程 | 成为这些模块的 owner |
| `shield_ops` | all | 只读观测、导出状态 | 反向控制语义、成为业务依赖 |

## `shield_cluster`

### 最终 public surface

`shield_cluster` 只拥有两类 public surface：

- **远端发现/观察**：`shield.cluster.*`
- **远端业务通信**：仍通过统一 `shield.send/call` 完成

最终规则：

- `shield.query(name)` 继续只查本地 registry。
- 远端 name 解析由 `shield.cluster.query(node, name)` 承担。
- 远端 node 观察由 `shield.cluster.nodes()` 承担。
- 一旦拿到 remote `ServiceHandle`，业务消息仍走 `shield.send/call`，不再定义 `shield.cluster.send/call`。
- 节点 connect/disconnect、peer 管理属于部署/配置/ops 责任，不作为业务 Lua API 暴露。

### 配置 owner

- `cluster`

### 语义 owner

- 节点身份：`NodeId`、`node_epoch`
- remote route cache
- node heartbeat：`online/suspect/offline/removed`
- 可选发现机制：static、broadcast、redis、kubernetes、etcd/consul

### 不允许的能力

- 把 global name registry 变成 core 默认能力
- 让 Lua 业务直接操作 CAF middleman
- 为业务消息再复制一套 cluster 专用 envelope 语义

## `shield_global`

### 最终 public surface

`shield_global` 拥有自己的业务命名空间：

```lua
shield.global()
shield.mutex()
shield.rwlock()
shield.spinlock()
shield.distributed_mutex()
shield.distributed_rwlock()
shield.rank()
shield.queue()
shield.rate_limiter()
shield.scheduler()
```

最终规则：

- `shield_global` 封装“常用跨进程模式”，不替代 `shield.db.*` / `shield.redis.*`。
- `shield_global` 不能暴露原始 Redis 连接、事务句柄或底层驱动对象。
- 分布式能力优先复用 Redis 语义；若未来支持其他后端，也属于模块内部实现细节。

### 配置 owner

- `global`

### 语义 owner

- 全局 KV / cache
- 分布式锁
- 排行榜
- 队列
- 限流器
- 定时任务调度

### 不允许的能力

- 反向修改 `shield_data` 的 raw API 契约
- 把 queue/rank/lock 语义写回 core
- 直接要求 cluster 必须存在

## `shield_player`

### 最终 public surface

`shield_player` 拥有：

- `shield.player.setup`：业务 Player Service 唯一推荐入口
- `shield.player.resolve`：`PlayerRef` 解析为本地 `PlayerSession`
- `shield.player.defaults`：可选钩子的默认实现 helper（业务覆盖时如需保留默认行为可显式调用）
- `shield.player.manager`：获取 `PlayerManager`
- `PlayerSession`：本地玩家会话对象
- `PlayerRef`：跨 service 轻量引用
- `PlayerManager`：全局玩家索引与批量操作
- player lifecycle hooks（通过 setup opts 注册）

最终规则：

- `shield_player` 是玩家态扩展，不改变普通 Lua service 的 module-table + named method 基础模型。
- player hooks 只在 player module 场景内生效，不替代普通 service method dispatch。
- `SessionHandle` 只留在 gateway / `shield_net` 内部映射中。
- `shield_player` 跨 service 传递**只能**使用 `PlayerRef`；不传 `SessionHandle`，也不传完整 `PlayerSession`。
- `PlayerRef` 不是 `ServiceHandle` 的替代品，只是 player 模块内部引用。
- persistence adapter 是 `shield_player` 可选能力，复用 `shield_data` 的 raw DB/Redis API，不引入 ORM 或 mapper；不拥有连接池。
- anonymous/spectator、多设备、player_pool 和远端 `PlayerRef` resolve 都必须显式启用；默认 player 状态机和普通 service 语义不变。
- gateway 可以不启用 `shield_player` 也独立工作。

### 配置 owner

- `player`
- `player_manager`

### 语义 owner

- 认证
- 登录/登出
- 重连窗口
- 离线消息
- 多设备策略
- anonymous/spectator 扩展状态
- player_pool 运行模型
- PlayerManager 索引
- persistence adapter 契约（不包含 SQL/Redis 语义）

### 不允许的能力

- 把玩家生命周期做成 core 必需能力
- 让 player hooks 污染普通 service 的 method 语义
- 用 `PlayerSession` 或 `PlayerRef` 取代 `ServiceHandle`
- 把 ORM、mapper 或 schema 工具链塞进 persistence adapter
- persistence adapter 自己拥有 DB/Redis 连接池

## `shield_server`

### 最终 public surface

`shield_server` 拥有：

- `shield.server()`
- `server_manager` service

最终规则：

- `shield_server` 负责服务器级状态，不负责 service runtime 基础语义。
- `shield_server` 可以触发维护模式和关服流程，但真正的 stop/drain 仍由 bootstrap/core 路径执行。
- 服务器状态通知是模块能力，不是全局事件总线回归。

### 配置 owner

- `server_manager`

### 语义 owner

- server state：`starting/running/maintenance/shutdown`
- 运行时信息：version、started_at、node_id
- 状态通知

### 不允许的能力

- 取代 `shield_bootstrap`
- 取代 `shield_ops`
- 接管玩家态或全局态对象所有权

## `shield_ops`

### 最终 public surface

`shield_ops` 不提供业务 Lua API。它的 public surface 是：

- HTTP 运维端点
- local admin socket / console
- metrics exporter
- profile 控制入口

最终规则：

- 业务 Lua service 不直接依赖 `shield_ops`。
- `shield_ops` 读取 `RuntimeSnapshot`、counters、module status，不反向定义语义。
- 控制类操作必须通过显式管理入口和鉴权完成，不通过普通业务 API 透出。

### 配置 owner

- `ops`

### 语义 owner

- health
- metrics
- diagnostics
- console
- profile

### 不允许的能力

- 把健康检查、metrics、console 变成 core 默认启动路径
- 默认暴露敏感 payload、token、密码
- 通过 ops 端点直接依赖 CAF 类型

## 模块关闭时的行为

各 optional module 关闭后必须满足：

- `shield_cluster` 关闭：本地 registry、local `send/call`、gateway、DB/Redis 都不受影响。
- `shield_global` 关闭：raw `shield.db.*`、`shield.redis.*` 仍可直接使用。
- `shield_player` 关闭：gateway 与普通 service 语义不受影响，只是没有玩家态框架能力。
- `shield_server` 关闭：runtime 仍可启动、运行、优雅关闭。
- `shield_ops` 关闭：runtime 仍可运行，只失去观测/管理入口。

## 错误策略

optional module 不能把所有模块结果都塞进统一 `ok == false` 通道。错误分层规则如下：

- **framework/runtime error**：模块未启用、后端连接丢失、节点离线、配置非法、snapshot 不可读等，走 `ok == false, Error` 或管理端点错误。
- **module contract negative result**：限流命中、锁竞争失败、玩家认证拒绝、队列为空、维护模式拒绝新登录等，优先作为模块自己的返回契约表达，不重定义 core runtime error。

分模块约束：

- `shield_cluster`：优先复用 `node_offline`、`timeout`、`module_unavailable` 等 runtime 错误；远端路由 stale 等 cluster 语义错误由 cluster 域自己拥有。
- `shield_global`：后端失败复用 DB/Redis 错误；锁竞争、限流、空队列等结果属于模块契约，不应伪装成 transport/runtime 故障。
- `shield_player`：认证拒绝、重复登录策略命中等属于 player 契约结果；framework 只负责 lifecycle hook 调度和模块可用性错误。
- `shield_server`：状态迁移非法属于 server 模块 API 错误；关服流程中的 runtime stop 失败仍属于 bootstrap/core 错误。
- `shield_ops`：HTTP/admin/console 错误停留在管理平面，不回流成业务 Lua API 错误。

## 验收要求

这些模块都必须满足：

- 有独立配置校验。
- 有独立错误归属说明。
- 有独立验收矩阵。
- examples 不能替代模块测试。

对应验收矩阵见 [官方可选模块验收矩阵](optional-module-tests.md)。
