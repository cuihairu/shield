# 核心设计理念

本文解释重构后的设计原则。当前仍是设计阶段，不代表源码已经全部调整到位。

具体运行时语义、API 返回值、ID、registry、payload、timer、Lua VM、ops 和 cluster 预留规则见 [运行时语义决策稿](./runtime-semantics.md)。

## 1. 单节点优先，集群可选

Shield 首先要把单节点游戏服务运行时做稳定。多节点部署、服务发现、集群路由、节点编排不进入 `shield_core`。

`shield_cluster` 是官方可选模块，用于多进程/多机器通信、节点心跳、远端路由 cache 和可选服务发现。它必须复用 core 的 service handle、send/call、timeout 和错误语义，不能把服务发现或集群编排反向塞回 core。

这个取舍的原因很直接：Shield 要先把服务、消息、网络、定时器、Lua 热迭代这些核心路径做薄、做稳，再在清晰边界上补集群能力，而不是继续扩张成通用企业框架。

## 2. Lua 优先

游戏逻辑默认写在 Lua 中。C++ 用于运行时、网络、Lua 绑定、底层数据访问和少量性能敏感扩展。

```lua
shield.send("room", "join", { player_id = "1001" })
local ok, result = shield.call("player", "get_info", { id = "1001" })
```

Lua 业务代码不应该感知 CAF、线程模型、连接对象或 C++ 容器细节。

## 3. CAF 是实现细节

CAF 处理 Actor 机制：spawn、send、request、schedule、调度等。Shield 在其上提供游戏服务器语义：

- 命名服务，而不是直接暴露 CAF actor handle。
- `send` / `call` 双模式。
- 服务生命周期钩子。
- 定时器和延迟任务。
- Lua 友好的 table 数据结构。

用户侧 API 不暴露 CAF 头文件。

一句话原则：

```text
CAF 覆盖机制，Shield 补服务语义。
```

CAF 可以承接 actor 调度、mailbox、spawn、send、request、timer 和远程 actor transport。Shield 仍然必须实现 service name registry、Lua coroutine pending/resume、Lua service 生命周期和 `shield.*` API。

> **当前实现状态（待纠偏）**：CAF service runtime 尚未闭环——`CafAdapter::call` 仍是 stub，Lua 服务实际由 `LuaServiceManager` 自管一套 mailbox + 单 worker runtime。这是与上述目标态的偏差，属架构债，纳入纠偏计划（见 [架构决策记录](architecture-decisions.md) AD-01、[roadmap](roadmap.md)）。本节描述的「CAF 是唯一 actor runtime 底座」仍是坚持的目标，不因当前偏差而改变。

## 4. 小核心

`shield_core` 只包含 Actor/Service 语义：

- 服务生命周期。
- 服务命名和查找。
- `send` / `call`。
- `spawn` / `exit`。
- timer / timeout。
- coroutine pending/resume。
- CAF adapter。

Lua、网络、transport、data、config、log、bootstrap 都是围绕 `shield_core` 的官方运行时模块，不是 core 语义本身。`cluster`、`global`、`ops`、`player` 等属于官方可选模块。

旧架构中的 DI、注解、条件装配、插件、Prometheus、健康检查、服务发现、事件总线、独立协议层等都不是 core。需要保留时必须有明确的非核心归属，不能进入 core 或默认启动主路径。

## 5. 消息就是事件

Actor 消息是唯一跨服务通信模型。不再额外设计事件总线来表达相同概念。

`shield.event` 如果存在，只能是当前 Lua service / 当前 Lua VM 内部的同步 observer 工具。它不进入 mailbox，不序列化 payload，不跨 service/VM/node，也不用于 application ready、shutdown 或 service lifecycle 编排。

推荐模式：

```lua
shield.send("chat", "broadcast", {
    channel = "world",
    text = "hello"
})
```

只有需要返回值时才使用 `shield.call`。

## 6. Gateway 是连接与 session 绑定边界

Gateway runtime 持有客户端连接和 session 绑定管理，不是通用 Lua 消息分发器。普通客户端 RPC 的固定链路是：

```text
frame decode -> header.route_id
  -> Gateway 轻量路由表校验
  -> session.target（AuthService 或 PlayerService）
  -> CAF ClientIngress
  -> target Service adapter: cached handler
  -> body decode
  -> concrete Lua RPC handler
```

route 信息只存在 wire header；body 只包含该 RPC 的业务参数。Gateway 不从 route_id 解析目标服务，不做多目标路由，不解普通业务 body。

CAF behavior 负责区分 `ClientIngress`、普通 service send/call、生命周期控制等内部消息类别；Shield Service adapter 负责把 header `route_id` 映射到目标 VM 中启动期缓存的 Lua handler。

所有客户端消息发给 session 绑定的目标服务（登录前 → AuthService，登录后 → PlayerService）。room/scene/map 的动态路由由 PlayerService 私有状态管理，不在 Gateway 层面参与。

## 7. 数据访问由插件提供

数据访问不进入 `shield_core`，也不再由独立 `shield_data` 模块承载。SQL、文档库、缓存、队列和排行榜等后端能力由插件系统 v1 提供；业务 Lua 通过插件 namespace 和 binding 逻辑名访问。

```lua
local db = shield.database.mysql("database.default")
local ok, rows = db:query("SELECT * FROM users WHERE id = ?", { id })

local q = shield.queue.redis("queue.events")
q:publish("chat.world", { text = "hello" })
```

插件负责连接池和驱动集成；core 不提供 ORM、不生成复杂 mapper、不管理跨服务事务。

## 8. 运维与调试分离

`shield_ops` 是官方可选运维与调试层，不属于 `shield_core`。

- `shield_core` 负责语义。
- `shield_ops` 负责 metrics、health、profile、diagnostics、console。
- `shield_ops` 可以读取内部状态，但不能反向依赖 core 语义。
- 业务代码不应该直接调用 `shield_ops`，只能通过管理端点或控制台使用它。
