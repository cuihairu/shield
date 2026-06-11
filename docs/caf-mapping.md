# CAF 映射

CAF 是 Shield 的内部 Actor 传输基础。重构后的原则是：**CAF 处理 Actor 机制，Shield 暴露游戏服务器语义**。

本文描述目标设计，不代表当前源码已经完成边界收敛。

具体 API 返回值、payload、coroutine 和 registry 语义见 [运行时语义决策稿](./runtime-semantics.md)。

## CAF 提供什么

| CAF 能力 | Shield 使用方式 |
| --- | --- |
| actor spawn / quit | 创建和停止内部 actor |
| send / request | 支撑 `shield.send` / `shield.call` |
| schedule / timeout | 支撑 timer API |
| scheduler | 支撑运行时调度 |
| serialization hooks | 仅作为内部实现细节 |

## CAF 覆盖 Skynet 的哪些机制

| Skynet actor 机制 | CAF 对应能力 | Shield 决策 |
| --- | --- | --- |
| 服务是运行单元 | `caf::event_based_actor` | 用 CAF actor 承载 Shield service |
| 异步消息 | `send` / `anon_send` | 封装成 `shield.send` |
| 请求-响应 | `request(...).then/await` | 封装成 `shield.call` |
| 创建服务 | `actor_system.spawn` | 封装成 `shield.spawn` |
| 服务退出 | actor quit / exit | 封装成 `shield.exit` |
| 定时器 | `schedule` / delayed message | 封装成 `shield.timer` / `shield.timer_once` |
| 远程 actor 通信 | `middleman` publish/connect | 不进 core，由 `shield_cluster` 模块内部使用 |

结论：CAF 足够覆盖 Actor 机制层，不需要 Shield 自己重写 actor 调度、mailbox、request、timer 或远程 actor transport。

## Shield 必须补的语义

CAF 不提供 Skynet 风格的 Lua runtime 语义。Shield 需要补：

| Shield 语义 | 为什么 CAF 不直接提供 |
| --- | --- |
| service name registry | CAF 主要操作 actor handle，不是游戏服务名 |
| Lua coroutine pending/resume | CAF request 不知道 Lua coroutine |
| `shield.call` 同步写法 | CAF 是 C++ continuation/request 机制 |
| `shield.service` 生命周期 | CAF 不知道 Lua 脚本和 `on_init/on_message/on_exit` |
| Lua table payload | CAF 倾向 C++ 类型系统 |
| `uniqueservice` / `queryservice` 类语义 | 这是 Skynet/Shield 的服务模型，不是 CAF 机制 |

## Shield 添加什么

| Shield 语义 | CAF 机制 |
| --- | --- |
| `shield.send(name, type, data)` | 查找命名服务后发送 actor message |
| `shield.call(name, type, data)` | 查找命名服务后 request / response |
| `shield.call_timeout(timeout_ms, name, type, data)` | 带显式超时的 request / response |
| `shield.spawn(module, opts)` | 创建 Lua 服务实例，init 成功后返回 ready handle |
| `shield.exit()` | 停止当前服务 |
| `shield.timer(...)` | 调度延迟或周期任务 |

## 不暴露 CAF

Lua 和用户侧 C++ 扩展不应该 include CAF 头文件，也不应该直接操作 CAF actor handle。

目标 Lua 代码：

```lua
shield.send("room", "join", { player_id = "1001" })
local ok, result = shield.call_timeout(3000, "player", "get_info", { id = "1001" })
```

## 分布式能力

CAF 自身有远程 actor 能力，但 Shield 当前 core 目标是单节点优先 runtime。服务发现、集群路由和节点编排不进入 core。

多节点能力归入 `shield_cluster` 官方可选模块，而不是通过 CAF 细节泄漏到用户 API。

## 参考

- CAF `event_based_actor`: https://www.actor-framework.org/static/doxygen/0.18.7/classcaf_1_1event__based__actor
- CAF requester/request: https://www.actor-framework.org/static/doxygen/0.18.7/classcaf_1_1mixin_1_1requester
- CAF middleman: https://www.actor-framework.org/static/doxygen/1.0.0/namespacecaf_1_1io
