# 核心设计理念

本文解释重构后的设计原则。当前仍是设计阶段，不代表源码已经全部调整到位。

具体运行时语义、API 返回值、ID、registry、payload、timer、Lua VM、ops 和 cluster 预留规则见 [运行时语义决策稿](./runtime-semantics.md)。

## 1. 单节点优先

Shield 首先是单节点游戏服务运行时。多节点部署、服务发现、集群路由、节点编排不进入 core。用户可以在业务层或外部基础设施中实现这些能力。

这个取舍的原因很直接：Shield 要先把服务、消息、网络、定时器、Lua 热迭代这些核心路径做薄、做稳，而不是继续扩张成通用分布式框架。

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

## 4. 小核心

`shield_core` 只包含 Actor/Service 语义：

- 服务生命周期。
- 服务命名和查找。
- `send` / `call`。
- `spawn` / `exit`。
- timer / timeout。
- coroutine pending/resume。
- CAF adapter。

Lua、网络、transport、data、config、log、bootstrap 都是围绕 `shield_core` 的官方运行时模块，不是 core 语义本身。

旧架构中的 DI、注解、条件装配、插件、Prometheus、健康检查、服务发现、事件总线、独立协议层等都不是 core。需要保留时必须有明确的非核心归属，不能进入 core 或默认启动主路径。

## 5. 消息就是事件

Actor 消息是唯一跨服务通信模型。不再额外设计事件总线来表达相同概念。

推荐模式：

```lua
shield.send("chat", "broadcast", {
    channel = "world",
    text = "hello"
})
```

只有需要返回值时才使用 `shield.call`。

## 6. 网关是 Lua 服务模式

网络层负责连接和字节流。业务网关应该是 Lua 服务，通过 `on_connect`、`on_disconnect`、`on_message(session, payload)` 等回调组织登录、会话和路由。

重构后不把 middleware chain 作为核心设计。鉴权、限流、CORS 等策略可以在 Lua gateway 服务或用户自己的 C++ transport 中实现。

## 7. 数据访问保持原始

`data` 模块可以提供 DB / Redis 原始能力，但不提供 ORM、不生成复杂 mapper、不管理跨服务事务。

```lua
local rows = shield.db.query("SELECT * FROM users WHERE id = ?", { id })
shield.redis.publish("chat:world", { text = "hello" })
```

如果将来需要更高级的数据访问，应作为可选扩展独立讨论。

## 8. 运维与调试分离

`shield_ops` 是官方运维与调试层，不属于 `shield_core`。

- `shield_core` 负责语义。
- `shield_ops` 负责 metrics、health、profile、diagnostics、console。
- `shield_ops` 可以读取内部状态，但不能反向依赖 core 语义。
- 业务代码不应该直接调用 `shield_ops`，只能通过管理端点或控制台使用它。
