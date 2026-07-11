# Shield 最终架构总纲

本文是 Shield 的**总架构蓝图**。它回答的不是“先做哪一阶段”，而是“最终要落成什么样的系统、模块边界如何划分、公共契约由谁拥有、哪些旧设计被永久删除”。

当前源码仍保留旧架构残留，因此本文描述的是**目标终态**，不是“源码已经全部实现”的声明。

当前最显著的偏差：CAF service runtime 尚未闭环，Lua 服务实际由 `LuaServiceManager` 自管 runtime。这是一项待纠偏的架构债，详见 [架构决策记录](architecture-decisions.md)（AD-01）与 [roadmap](roadmap.md)。本文仍以「CAF 是唯一 actor runtime 底座」为目标态，不因当前实现偏差而改动目标。

更细的运行时语义、Lua API、配置 schema、错误码和可选模块细节分别见对应专题文档；但模块边界、依赖方向、对象归属和扩展规则以本文为总纲。

## 文档角色

本文负责冻结以下问题：

- Shield 的最终模块分层。
- `shield_core`、运行时模块、官方可选模块、用户业务代码之间的边界。
- 关键运行时对象由谁拥有、在哪一层可见、是否允许跨 service 传递。
- 公共 Lua API、公共 C++ API、配置段、错误语义分别由谁定义。
- 哪些旧架构方向被永久删除，不再作为兼容目标。

本文不重复定义所有函数细节；函数级契约仍以 [Lua API 契约](lua-api.md) 和各专题语义文档为准。

## 设计目标

Shield 的最终目标是一个**Lua 优先、单节点最小部署清晰、可通过官方模块显式扩展到多节点与运维能力**的游戏服务运行时。

核心目标：

- 小核心：`shield_core` 只承载 service/message/timer/coroutine 语义。
- Lua 优先：业务逻辑默认写在 Lua service 中。
- CAF 隐藏：CAF 只作为内部 actor 机制，不进入 public API。
- 显式扩展：cluster/global/player/server/ops 都是显式模块，不反向污染 core。
- 契约优先：Lua API、配置 schema、错误语义、测试矩阵先冻结，再映射到实现。
- 删除旧债：DI/IoC、注解、条件装配、旧 Lua 插件执行模型（v0）、事件总线、中间件链等旧方向不再保留兼容层。插件系统 v1 是新设计的扩展机制，与旧 v0 不兼容。

## 基本术语

| 术语 | 含义 |
| --- | --- |
| service | Shield 的基本执行单元；对用户可见的是命名 service 和 Lua module method |
| actor | 实现层术语；Shield 内部可基于 CAF actor，但不直接暴露给用户 |
| runtime module | `shield_core` 之上的官方运行时模块，如 `shield_lua`、`shield_net`、`shield_plugin` |
| optional module | 官方可选模块，如 `shield_cluster`、`shield_global`、`shield_player`、`shield_server`、`shield_ops` |
| gateway service | 具备网络回调的 Lua service 模式，而不是独立框架层 |
| public contract | 用户必须依赖且后续实现必须满足的契约；主要包括 Lua API、public C++ header、配置 schema、错误语义 |

## 最终分层

```text
┌──────────────────────────────────────────────────────────────┐
│                       User Lua Services                      │
│        scripts/*.lua + module-table handlers + game logic   │
├──────────────────────────────────────────────────────────────┤
│                     Public Lua API Surface                   │
│  shield.* / plugin namespaces / optional namespaces          │
├──────────────────────────────────────────────────────────────┤
│                  Official Optional Modules                   │
│  shield_cluster · shield_global · shield_player             │
│  shield_server · shield_ops                                 │
├──────────────────────────────────────────────────────────────┤
│                  First-Party Runtime Modules                 │
│  shield_lua · shield_net · shield_transport · shield_plugin │
│  shield_config · shield_log · shield_bootstrap              │
├──────────────────────────────────────────────────────────────┤
│                         shield_core                          │
│      service registry · message routing · timers            │
│      coroutine pending/resume · CAF adapter                 │
├──────────────────────────────────────────────────────────────┤
│                         shield_base                          │
│      Result · Error · ByteBuffer · time · basic IDs         │
├──────────────────────────────────────────────────────────────┤
│                             CAF                              │
│               actor scheduling / messaging / transport       │
└──────────────────────────────────────────────────────────────┘
```

这个分层表达的是**最终结构**，不是默认每次都把所有模块一起启用。

最终部署形态允许有两类：

- **最小部署路径**：`shield_base` + `shield_core` + 必要运行时模块（Lua/config/log，以及按需启用 net/plugin）
- **扩展部署路径**：在最小路径之上显式加入 cluster/global/player/server/ops 等官方可选模块

## 模块清单与边界

### 核心层

| 模块 | 负责 | 对外暴露 | 明确不负责 |
| --- | --- | --- | --- |
| `shield_base` | 基础值类型、错误对象、时间、buffer、ID，以及可被 Lua runtime / transport / CAF adapter 共用的纯原语基础 | 基础 C++ 类型 | 业务语义、Lua 绑定、网络、数据 |
| `shield_core` | service lifecycle、registry、message envelope、`send/call/spawn/exit` 语义、timer、coroutine pending/resume、CAF adapter | `ServiceHandle`、核心 service/message/timer 语义 | Lua VM、Session、DB/Redis、配置加载、日志、cluster 发现 |

### 运行时模块层

| 模块 | 负责 | 对 Lua / 用户可见 | 明确不负责 |
| --- | --- | --- | --- |
| `shield_lua` | Lua VM 生命周期、Lua service loader、`shield.*` 绑定，以及基础组件 Lua 表面（如未来的 `shield.buffer` / `shield.crypto` / `shield.socket`） | 主 Lua API 契约 | 网络监听、DB 驱动、cluster 路由 |
| `shield_net` | 连接生命周期、Session 管理、listener、网络背压 | gateway 回调、session 发送 | 业务鉴权策略、玩家状态管理 |
| `shield_transport` | Phase 1 basic framing/codec/encryption；UDP/KCP/WebSocket 适配为后续 transport extension | 通常不直接暴露给 Lua | service 语义、业务路由 |
| `shield_plugin` | 插件 manifest/catalog、实例生命周期、binding 解析、C ABI / Lua 注册分发 | `shield.plugin.*` introspection、插件自注册的 `shield.<namespace>` | 具体数据库/Redis 驱动、业务数据模型 |
| `shield_config` | YAML 加载、环境变量展开、schema 校验 | `shield.config(path)` 读取 | service registry、Lua VM |
| `shield_log` | 日志 sink、级别、结构化输出、轮转 | `shield.log.*` | metrics、profile、health |
| `shield_bootstrap` | Starter 组合、模块初始化顺序、`shield::run` | 稳定入口 `shield::run` | 业务逻辑、service 语义定义 |

### 官方可选模块层

| 模块 | 负责 | 允许依赖 | 明确不允许 |
| --- | --- | --- | --- |
| `shield_cluster` | 多进程/多机器通信、节点身份、远端路由 cache、节点心跳、可选发现 | `shield_core` 公共 handle/message 语义，必要运行时快照 | 改写本地 registry 规则、把发现机制塞进 core |
| `shield_global` | 跨进程共享数据、分布式锁、排行榜、队列、限流器、调度 | 数据插件 binding、`shield_cluster`（可选） | 直接拥有 DB/Redis 驱动实现、回写 core 配置 |
| `shield_player` | 玩家认证、登录、重连、离线消息、PlayerManager | `shield_net`、`shield_lua`、数据插件 binding、`shield_global`（可选） | 暴露 `SessionHandle` 跨 service 传递 |
| `shield_server` | 服务器状态、维护模式、关闭流程、状态通知 | `shield_core`、`shield_ops` 快照接口 | 替代 core bootstrap、接管玩家生命周期 |
| `shield_ops` | metrics、health、diagnostics、console、profile | runtime snapshot、只读计数器、模块公开状态接口 | 反向控制 core 语义、成为业务依赖 |

## 依赖方向

最终允许的依赖方向只有这一条主链：

```text
CAF
  -> shield_base
  -> shield_core
  -> first-party runtime modules
  -> official optional modules
  -> user services / executable composition
```

禁止规则：

- `shield_core` 禁止依赖 `shield_lua`、`shield_net`、`shield_plugin`、`shield_config`、`shield_log`、`shield_bootstrap`。
- `shield_core` 禁止在 public header 中暴露 CAF 类型。
- Lua-facing 基础组件 API 与 CAF transport adapter 只能共享纯原语层，不应互相复用对方的表面对象模型；见 [基础组件与运行时适配边界](runtime-primitives.md)。
- `shield_bootstrap` 可以组合各模块，但任何模块都不能反向依赖 `shield_bootstrap`。
- `shield_ops` 只能读取 runtime snapshot / counters / module status，不能成为 core 语义前置依赖。
- `shield_cluster` 可以扩展远端路由，但不能改变 `ServiceHandle`、本地 registry 和本地 `send/call` 的基础规则。
- `shield_player`、`shield_server`、`shield_global` 不能回写 core API 契约，只能在自己的命名空间上扩展能力。

## 基础组件边界

除 `service/message/timer` 之外，Shield 后续可能还会补一组更底层的 runtime primitives。它们的定位不是业务框架，而是给 Lua runtime、transport 和 CAF adapter 共用的基础原语。

推荐边界：

- 纯原语层：`buffer`、`encoding`、`crypto`、`codec core`、`tls common`
- Lua 运行时表面：`shield.buffer`、`shield.encoding`、`shield.crypto`、`shield.socket`、`shield.stream`、`shield.http`
- 应用层或插件层：`auth.jwt`、OAuth、Redis/MySQL/MongoDB 客户端、业务 RPC client

核心原则：

- core 提供原语，不提供业务策略
- Lua-facing API 与 CAF adapter 只共享纯原语，不共享对象模型
- `auth` 不应成为 core 中心抽象，`crypto` 才是更合理的基础层

这条线当前已降为后置草案，不属于当前主路径。详见 [基础组件与运行时适配边界](runtime-primitives.md)。

## 公共契约所有权

Shield 的公共契约按“谁拥有、谁定义、谁测试”分配：

| 契约 | owner | 权威文档 | 测试归属 |
| --- | --- | --- | --- |
| service/message/timer 基础语义 | `shield_core` | [runtime-service.md](runtime-service.md)、[runtime-messaging.md](runtime-messaging.md) | core 级单元/集成测试 |
| Lua 用户 API | `shield_lua` | [lua-api.md](lua-api.md) | [lua-api-tests.md](lua-api-tests.md) |
| gateway callback 与 Session 语义 | `shield_net` + `shield_lua` | [runtime-network.md](runtime-network.md)、[gateway.md](gateway.md) | network/gateway 测试 |
| 数据插件 API | `shield_plugin` + 各插件 | [runtime-data.md](runtime-data.md)、[plugin-system.md](plugin-system.md)、[plugins/index.md](plugins/index.md) | 插件系统与各插件测试 |
| core config schema | `shield_config` + `shield_bootstrap` | [runtime-config.md](runtime-config.md) | config schema 测试 |
| bootstrap 生命周期 | `shield_bootstrap` | [starter-system.md](starter-system.md)、[runtime-bootstrap.md](runtime-bootstrap.md) | bootstrap 测试 |
| optional module API | 各 optional module 自己 | 对应 runtime 文档 | 各模块独立测试矩阵 |

规则：

- 一个 public contract 只能有一个权威 owner。
- 示例文档和 hello_world 不能替代契约测试。
- 可选模块的 API 不能偷用 core 文档里的空白区域，必须写进自己的契约文档。

## 核心对象模型

### 对象总览

| 对象 | owner | 可见范围 | 是否可跨 service 传递 | 说明 |
| --- | --- | --- | --- | --- |
| `ServiceHandle` | `shield_core` | Lua + C++ | 可以 | opaque service reference，不暴露 CAF |
| `ServiceRegistry` | `shield_core` | runtime 内部 | 不适用 | 管理 name reserve/publish/query/unregister |
| `MessageEnvelope` | `shield_core` | runtime 内部 / 测试可见 | 可以编码 | 承载 method、argc、args、trace、deadline 等 |
| Lua VM | `shield_lua` | runtime 内部 | 不可以 | 默认按 service 生命周期管理 |
| `SessionHandle` | `shield_net` | gateway callback + 网络层内部 | 不可以 | 只在网络语义内使用；不跨 service 传递（见 AD-06） |
| `PlayerSession` | `shield_player`（可选，未实现） | 仅 `shield_player` 模块及业务 Lua | 不作为跨 service 对象传递 | 见 AD-02/AD-03：玩家状态归属 PlayerService 私有 Lua table；`shield_player` 与 PlayerSession 属可选模块范畴，非 core 对象 |
| `ServiceAddress` / `NodeId` | `shield_core` / `shield_cluster` | Lua + C++（按 handle 封装） | 可以 | cluster 只扩展地址语义，不改 core handle 规则 |
| `BootstrapContext` | `shield_bootstrap` | Starter 内部 | 不可以 | 显式 bootstrap 组合上下文，不是 service locator |
| `RuntimeSnapshot` | `shield_core` + modules | `shield_ops`、调试、测试 | 只读导出 | 观测用快照，不参与业务路由 |

### 对象边界规则

- `ServiceHandle` 是唯一跨 service 的运行时引用对象；`send/call` 的目标永远是 Service（见 AD-02）。
- `SessionHandle` 不通过 `shield.send/call` payload 跨 service 传递；跨 service 只传 `session_id`（见 AD-06）。
- `PlayerSession` 属可选模块对象，不进入 core 和最小 Lua API 主线；第一阶段玩家状态是 PlayerService 私有 Lua table，不另起运行时对象（见 AD-03）。
- `BootstrapContext` 只存在于启动阶段，不泄漏到 service 运行阶段。
- `RuntimeSnapshot` 只读，不可作为反向控制接口。

## 运行时主流程

### 1. 启动与组装

```text
shield::run(argc, argv)
  -> shield_config 读取并校验 core schema
  -> shield_log 初始化
  -> shield_core 初始化 service/message/timer 机制
  -> shield_plugin / shield_net / optional modules 按配置显式启用
  -> ScriptStarter 创建 Lua VM 策略并注册 core shield.* API
  -> shield_plugin 将已启动实例的 Lua namespace 注册到各 Lua VM
  -> ServiceStarter 按 actors 配置启动 Lua/C++ services
  -> network listeners 开始接受连接
  -> runtime 进入 steady state
```

启动原则：

- 组装顺序由 `shield_bootstrap` 明确控制。
- Starter 之间通过 `BootstrapContext` 读取显式字段，不通过全局 `ApplicationContext` 查询依赖。
- `ScriptStarter` 是 Lua VM 和 Lua API 绑定的唯一入口。

### 2. Service 到 Service

```text
caller service
  -> shield.send / shield.call
  -> shield_lua 打包 Lua 参数
  -> shield_core 路由到 target service
  -> target Lua module method dispatch
  -> response / error / timeout 按 core 语义回到 caller coroutine
```

规则：

- handler 参数只接收业务参数，不额外塞 `src`。
- 调用方上下文通过 `shield.sender()`、`shield.trace()`、`shield.deadline()` 读取。
- `shield.call` 挂起当前 Lua coroutine，但不阻塞 runtime 线程。

### 3. Client 到 Gateway 到 Service

```text
socket bytes
  -> shield_net connection/session
  -> shield_transport
     -> Envelope
     -> RouteExtractor
     -> RoutePolicy
     -> BodyCodec
  -> gateway service on_client_message(session, client)
     -- client = { route = "c2s.player.move", payload = {...} }
  -> gateway 按 session_id 绑定，用 shield.send/call 路由到 PlayerService 或业务 service
  -> PlayerService 经 shield.send(gateway, "client_send", {session_id, route, payload}) 回送
  -> gateway 依据 session_id 回写客户端
```

规则：

- gateway 是 Lua service 模式，不是独立框架层。
- transport pipeline 形状固定，不是业务可任意拼接的 middleware chain。
- 网络层只负责连接、session、背压和 transport 适配。
- 认证、路由、会话表、玩家派发策略由 gateway / player 模块承担。
- 客户端消息形状为 `client = { route, payload }`，codec 无关（见 AD-05）；`SessionHandle` 只在 gateway 内部使用，PlayerService 只持有 `session_id`（见 AD-06）。

### 4. Cluster 扩展路径

```text
local send/call
  -> shield_cluster 识别 remote target
  -> remote route cache / node state
  -> CAF remote transport
  -> remote node shield_core
  -> remote service dispatch
```

规则：

- 本地和远端尽量复用同一 `send/call/timeout/error` 语义。
- remote name 只是 cluster route cache，不进入本地 core registry。
- 节点发现属于 `shield_cluster`，不是 `shield_core` 的职责。
- 默认跨服模型是逻辑 service 通信，不是客户端 raw frame 横向转发。

推荐把跨服路径分成两类：

- 主路径：`shield.send/call` 驱动的业务消息路由
- 例外路径：`ForwardRaw` 驱动的协议数据面透传

后者只适合 proxy/gateway/协议兼容场景，不应成为业务层的常规协作模型。

### 5. Ops 观测路径

```text
shield_core + modules
  -> runtime snapshot / counters / module status
  -> shield_ops
  -> http endpoints / console / exporter / profile
```

规则：

- `shield_ops` 读取状态，不定义业务 API。
- 默认不暴露完整 payload、密钥、token、密码等敏感信息。

## 配置归属

最终配置分为 core schema 和 optional module schema 两层。

### Core Schema

| 配置段 | owner | 说明 |
| --- | --- | --- |
| `app` | `shield_bootstrap` | 应用名、版本等 runtime 基础信息 |
| `log` | `shield_log` | 日志级别、格式、sink、轮转 |
| `lua` | `shield_lua` | VM 模式、sandbox 限制 |
| `actors` | `shield_bootstrap` + `shield_lua`/`shield_net` | 启动的 service 列表、network 声明、资源限制 |
| `plugins` | `shield_plugin` | 插件目录、实例、binding；各实例 config 由对应插件 manifest schema 拥有 |
| `bootstrap` | `shield_bootstrap` | 初始化超时、retry |
| `shutdown` | `shield_bootstrap` | drain/stop/close 超时 |

### Optional Module Schema

| 配置段 | owner |
| --- | --- |
| `cluster` | `shield_cluster` |
| `global` | `shield_global` |
| `ops` | `shield_ops` |
| `player` / `player_manager` | `shield_player` |
| `server_manager` | `shield_server` |

规则：

- core 不解释 optional module 配置字段。
- optional module 不得反向扩展 core schema。
- 任何新增 public 配置段都必须明确 owner，并补独立文档与校验规则。

## 错误归属

错误语义也必须按模块归属，避免所有错误都堆到同一层。

| 错误域 | owner | 示例 |
| --- | --- | --- |
| service/message/timer | `shield_core` | `service_not_found`、`timeout`、`method_not_found`、`handler_error` |
| Lua load/binding/context | `shield_lua` | `invalid_service_module`、`script_load_failed`、`context_expired` |
| network/session/gateway | `shield_net` | `session_closed`、`backpressure`、协议相关错误 |
| 数据插件 | 各数据插件 / `shield_plugin` | `module_unavailable`、`db_query_failed`、`redis_command_failed` |
| bootstrap/config | `shield_bootstrap` / `shield_config` | 配置校验失败、模块初始化失败 |
| cluster | `shield_cluster` | `node_offline`、`remote_route_stale` 等远端语义错误 |
| optional module business domain | 各 optional module | player/global/server 自有错误 |

规则：

- optional module 可以复用 core 错误，但不能重定义 core 错误含义。
- 错误对象结构统一，但错误码归属必须清楚。

## 官方可选模块的最终边界

### `shield_cluster`

最终定位：

- 提供跨进程/跨机器 service 路由。
- 管理 node identity、heartbeat、remote route cache、可选发现机制。

最终不做：

- 不把服务发现做成 core 默认能力。
- 不改变本地 `ServiceRegistry` 规则。
- 不暴露 CAF middleman 给业务 Lua。

### `shield_global`

最终定位：

- 在显式配置的数据插件 binding（通常是 Redis 系插件）之上封装常见跨进程共享能力。

最终不做：

- 不接管 DB/Redis 连接池和底层驱动。
- 不修改数据插件业务接口（如 `shield.cache.v1` / `shield.queue.v1` / `shield.leaderboard.v1`）的基础契约。

### `shield_player`

最终定位：

- 提供认证、重连、离线消息、PlayerManager、PlayerSession 等玩家态能力。

最终不做：

- 不进入最小 Lua API 主线。
- 不把 `SessionHandle` 变成可跨 service 业务对象。

### `shield_server`

最终定位：

- 提供服务器级状态、维护模式、关服流程、运行时信息发布。

最终不做：

- 不替代 `shield_bootstrap`。
- 不把运维控制语义塞进 core。

### `shield_ops`

最终定位：

- 提供 metrics、health、console、profile、diagnostics。

最终不做：

- 不作为业务 service 的依赖。
- 不成为 runtime 正常运行的必要前置条件。

## 扩展点

Shield 只保留少量显式扩展点。

### Lua Service

默认业务扩展方式。服务以返回 table 的 Lua module 表达，通过 named method 暴露消息处理逻辑。

### C++ Service

只适合性能敏感或基础设施场景。C++ service 复用 `shield_core` 语义，但不能绕开 `ServiceHandle`、registry 和消息契约。

### Transport

适合编解码、解帧、加密、KCP 等字节流层扩展。Phase 1 只要求 basic framing/codec/encryption 与 TCP session 组合可用；UDP/KCP/WebSocket 属于后续 transport extension。Transport 只处理网络字节和固定 protocol pipeline 部件适配，不承载业务生命周期。

固定 protocol pipeline 的典型槽位是：

- `Envelope`
- `RouteExtractor`
- `RoutePolicy`
- `BodyCodec`
- `RouteResolver`

`json`、`msgpack`、`protobuf`、`sproto`、`xmldef` 等协议应作为这些槽位上的实现来接入，而不是各自带一套独立业务链路。

### Optional Module

适合明确属于官方扩展层且会长期维护的能力。新增 optional module 必须回答：

- 它为什么不属于 `shield_core`？
- 它依赖哪些已公开 contract？
- 它拥有哪些配置段、错误码和测试矩阵？
- 它是否能完全关闭而不影响最小部署路径？

## 永久删除的旧方向

以下方向不再进入最终架构，也不保留兼容层：

- `ApplicationContext` 全局组件注册中心
- DI / IoC 容器
- 注解装配、条件装配
- 旧 Lua 插件执行模型和旧 `shield.plugin.list/by_type/loaded/capabilities`
- C++ / Lua 生命周期事件总线
- middleware chain 作为框架级策略层
- discovery / metrics / health 进入 core
- ORM、复杂 schema mapper、跨 service 分布式事务
- 旧 `shield.service("name")`
- 旧 `on_message(src, type, data)` 统一入口
- 旧 `shield.db.*` / `shield.redis.*` 全局数据 API，包括冒号式 `shield.db:query(...)` / `shield.redis:get(...)`

插件系统 v1 的 `shield.plugin.*` 是新的扩展机制入口，提供 package/instance/binding introspection 与 plugin manifest 驱动的能力注册（数据库/缓存/队列/认证等）。它与上方"旧 Lua 插件执行模型（v0）"是两套独立设计：v0 已永久删除，v1 的权威契约见 [插件系统](plugin-system.md)。

这里删除的是跨模块、跨 service、跨 VM 的 lifecycle event bus，不禁止两类受限机制：runtime 直接调用的 service hook（如 `on_init/on_shutdown/on_exit`），以及单个 Lua VM 内部的本地 `shield.event` observer 工具。后者不能用于 application ready、shutdown 或 service lifecycle 编排。

## 文档地图

建议阅读顺序：

1. 本文：总边界、模块关系、对象归属
2. [Lua API 契约](lua-api.md)：用户 API
3. [插件系统](plugin-system.md)：扩展机制与 C ABI 契约
4. [插件参考](plugins/index.md)：官方插件清单
5. [官方可选模块契约](optional-modules.md)：optional module 横向边界
6. [配置运行时语义](runtime-config.md)：core schema
7. [Starter 系统](starter-system.md)：bootstrap 组合方式
8. [运行时语义决策稿](runtime-semantics.md)：专题索引与实现建议
9. [官方可选模块验收矩阵](optional-module-tests.md) 与 cluster/global/player/server/ops 专题文档

最终要求只有一句话：

```text
实现可以分批推进，但最终架构边界必须先于实现被明确冻结。
```
