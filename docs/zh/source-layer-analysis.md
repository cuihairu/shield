---
title: 源码分层分析
---

本页按源码证据归纳职责边界；层数表达语义边界数量，不是 `src/` 目录映射。证据路径对应提交 `e588c0e0a95800596aaefe3309d684944c20b15a`。

## 执行角色

| 层 | 职责 | 状态存储 | 入站通道 | 出站通道 | 配置输入 | 生命周期 |
| --- | --- | --- | --- | --- | --- | --- |
| 进程装配 | 校验配置，按序构造并补偿所有子系统。 | bootstrap 持有的进程全局状态。 | CLI 参数；`Phase` starter。 | 直接构造调用；失败清理回调。 | `config/*.yaml`；CLI 覆盖。 | `initialize` 到受保护清理，再到 `shutdown`。 |
| 配置总线 | 解析、合并、校验并暴露运行时 YAML。 | 读写锁保护的 `Config::Impl`；进程单例。 | YAML 文件；bootstrap。 | `get*`、actor/runtime 访问器、插件子树 JSON。 | 其自身文件即运行时配置 schema 来源。 | 加载、重置、校验；该路径无热更新。 |
| Lua 服务域 | 每个服务拥有一个 actor 串行化的 VM 和协程 mailbox。 | 服务/VM/协程/定时器/profile 注册表；进程内存。 | CAF `ServiceMessage`、`ClientIngress`、控制和生命周期消息；插件 post/resume 钩子；console eval。 | CAF call/reply；网关 egress；插件 Lua 调用；经 cluster 的远端 envelope。 | `lua.*`、`actors[]`、`bootstrap.*`、`shutdown.*`。 | spawn/init、ready 前暂存、dispatch、drain、exit、强移、manager 关停。 |
| 客户端网关 | 执行网络准入，并把客户端 RPC 路由到一个绑定目标。 | `TcpListener` session 表、每 session 的 `SessionBinding`、gateway 弱引用注册表、路由表。 | TCP/TLS 包；HTTP server 请求；Lua bridge 回调。 | CAF ingress/control；session 写出；`ClientEgress`。 | 每个 actor 的 `network`、`network.protocol`、`network.tls`，以及 `console`、`http`。 | listener/session 启动；actor 处理连接、包、断开；逆序停止。 |
| 可插拔能力 | 解析、加载、校验、启动并暴露具名 provider 实例。 | package/instance/binding 注册表；`dlopen` 句柄；插件自有连接池。 | YAML `plugins`；bootstrap；Lua namespace；依赖 vtable。 | host API 回调；依赖 vtable；`register_lua`；Lua suspend/resume。 | `manifest.yaml` 与实例 `config_schema`；`plugins.instances/bindings`。 | 扫描→编目→编排/解析→加载→创建→启动；逆序关停且不提前 `dlclose`。 |
| 后端提供者 | 持有具体外部资源并暴露具名能力。 | Redis 连接池/流、数据库 worker/pool 状态、codec 状态、插件进程内存。 | plugin host；Lua namespace；依赖 vtable；HTTP/probe 调用。 | Redis、MySQL、PostgreSQL、SQLite、MongoDB、Prometheus、HTTP probe。 | manifest schema 加 provider 专属配置。 | create、start/connect、注册 Lua、shutdown/disconnect/drain；可选实例失败可降级。 |
| 可选运行模块 | 在 core 外提供跨服务状态、玩家身份、服务器状态和节点协作。 | `PlayerManager`、`ServerManager` mutex 表；`ClusterManager` 节点/路由状态；调度器/队列内存；可选 Redis。 | Lua facade；bootstrap；transport 回调。 | Redis；CAF middleman；注入的 notify/stop 回调；Lua task fire。 | 模块自持的 `global`、`cluster`、`player`、`server_manager` 分区。 | bootstrap start/stop；scheduler tick 和 shutdown timer。 |
| 运维入口 | 读取状态并调用显式启用的控制动作。 | 内嵌 SPA 和静态文件；profile/eval 记录。 | HTTP；Unix console session。 | 配置/插件/服务/全局管理器；Lua dispatch；JSON 响应。 | `http.*`、console token、静态根目录。 | bootstrap 注册路由；HTTP/console 最先停止。 |
| 管理端应用 | 呈现轮询到的运维快照。 | `sessionStorage` bearer token；浏览器组件状态。 | 用户操作；HTTP JSON。 | 携带认证头、可取消且带退避的 fetch。 | Vite 构建配置；运行时 token 输入。 | 浏览器页面生命周期。 |

## 实现落位

### 基础、诊断与消息契约

`shield_base` 持有无依赖的 ID、时间、缓冲区、`Result` 与 `Error` 类型（`include/shield/base/base.hpp:5`，`include/shield/base/error.hpp:11`）。CAF 类型注册表位于 `shield_core`（`src/caf_initializer.cpp:1`）；`include/shield/core/service_message.hpp:46` 定义本地服务与客户端消息，`include/shield/cluster/cluster_messages.hpp:22` 定义跨进程序列化 wire 类型。日志是 sink facade，提供控制台、文件和滚动文件实现（`include/shield/log/logger.hpp:14`，`include/shield/log/sinks.hpp:13`）；Crashpad 仅在启用时启动可重启 handler，默认只写本地 dump 并警告上传仍是留位（`src/crash/crash.cpp:31`，`src/crash/crash.cpp:82`）。这些是共享契约，不是用户业务层。

### 进程装配与配置总线

`Bootstrap` 是组合根：初始化日志、加载 YAML、运行 `PRE_INIT`/`POST_CONFIG` starter、校验编译内模块可用性，然后按序启动插件、可选模块、CAF、Lua 服务、监听器、console 和 HTTP（`src/bootstrap/bootstrap.cpp:402`，`src/bootstrap/bootstrap.cpp:508`）。部分失败会反向清理已启动的 listener、server、global、transport、service、console 和插件上下文（`src/bootstrap/bootstrap.cpp:306`）。关停先停可观测入口，再 drain Lua，最后逆序释放模块、插件与资源所有权（`src/bootstrap/bootstrap.cpp:1638`）。

配置总线不只是读文件：`Config` 在锁内持有 YAML 根和扁平化键值（`include/shield/config/config.hpp:14`），文件初始化在 `src/config/config.cpp:1111`，`validate_runtime_config` 在消费者启动前阻止未编译模块的分区（`src/config/config.cpp:1115`）。模块 schema 保留在模块解析器旁，例如 `GlobalConfig::from_global_config`（`src/global/global_manager.cpp:239`）和 `PlayerConfig::from_global_config`（`src/player/player_manager.cpp:36`）。

### Lua 服务域

Lua 层是应用执行域。`LuaRuntime` 持有 VM 策略并构造 service manager（`src/lua/lua_runtime.cpp:1`）；`LuaServiceManager` 对服务内 dispatch 串行化，实现 send/call/session 完成、drain/exit、timer、异步插件完成、命名、profile 和强移（`src/lua/lua_service.cpp:2884`，`src/lua/lua_service.cpp:3397`）。`shield.*` 是用户契约：service/message、timer/task、config/log、client、HTTP server、crypto、plugin introspection 和可选模块 facade 在同一组合点注册（`src/lua/lua_api.cpp:5145`）。gateway 和 HTTP bridge 都把各自通道转成 actor 任务，避免网络线程直接触碰 VM（`src/lua/lua_gateway_bridge.cpp:46`，`src/lua/lua_http_bridge.cpp:80`）。

### 客户端网关

`TcpListener` 持有 accept、连接/IP 限制、blocklist、TLS 握手、速率限制、帧上限、session 注册表和指标（`include/shield/net/listener.hpp:76`）。`TcpSession` 存储单目标 `SessionBinding`、单调 epoch、发送队列、token bucket 和类型化发送入口（`include/shield/net/session.hpp:107`，`include/shield/net/session.hpp:131`）。传输管线解析 envelope、解析 header/body route 元数据、按需经 codec 解码并应用路由策略（`src/transport/protocol.cpp:1420`，`src/transport/protocol.cpp:1622`）。gateway bridge 随后校验方向、认证和路由，只向 session 已绑定目标发送一个类型化 ingress，不按 body 或 actor 地址路由（`src/lua/lua_gateway_bridge.cpp:81`）。独立的 gateway actor 是改绑、关闭和客户端 egress 的唯一变更点（`include/shield/lua/gateway_actor.hpp:1`）。HTTP 是另一条请求/响应入口：`HttpServer` 分发，ops 与 Lua-HTTP bridge 分别拥有路由语义（`include/shield/net/http_server.hpp:31`，`src/lua/lua_http_bridge.cpp:80`）。

### 可插拔能力与后端提供者

`PluginHost` 执行扫描、编目、编排/解析、加载、创建、启动、Lua 注册和逆序关停（`src/plugin/plugin_host.cpp:1066`，`src/plugin/plugin_host.cpp:1118`）。包由 manifest 描述；host 在使用本地代码前校验 ABI 身份与实例配置（`include/shield/plugin/abi.h:33`，`src/plugin/plugin_host.cpp:266`，`src/plugin/plugin_host.cpp:289`）。`shield_host_api_v1` 只暴露实例配置、显式依赖、Lua 路径/注册和异步 resume 原语（`include/shield/plugin/host_api.h:26`）。提供者单独成层，因为它们拥有外部套接字与驱动：Redis driver/cache/queue/leaderboard 使用共享池或受控 fallback（`plugins/cache.redis/shield_cache_redis.cpp:969`，`plugins/queue.redis/shield_queue_redis.cpp:1058`）；SQL/文档包持有 worker pool 和事务 lane（`plugins/mysql/shield_db_mysql.cpp:1796`，`plugins/mysql/shield_db_mysql.cpp:2042`）。

### 可选运行模块

`GlobalManager` 通过独立 backend 实现 KV/cache、锁、排行榜三域，可选进程内存或 Redis（`src/global/global_manager.cpp:319`），而队列、调度器和限流器留在进程内（`src/global/global_manager.cpp:1196`，`src/global/global_manager.cpp:1343`）。cluster transport 通过 CAF middleman 发布/拨号 peer，承载认证 hello、heartbeat、路由快照和 envelope（`include/shield/cluster/cluster_transport.hpp:3`，`include/shield/cluster/cluster_messages.hpp:22`）；discovery 通过 Redis 注册和扫描节点（`include/shield/cluster/node_discovery.hpp:87`）。`PlayerManager` 刻意只做本地准入/重连/设备索引，不是 actor 或持久层（`include/shield/player/player_manager.hpp:79`，`src/player/player_manager.cpp:102`）。`ServerManager` 持有合法状态迁移表与 watcher/stop 回调（`include/shield/server/server_manager.hpp:20`，`src/server/server_manager.cpp:60`）。

### 运维入口与管理端应用

`OpsHttpHandler` 把 health、status、metrics、services、plugins、config、SPA、opt-in eval 和 opt-in profile 路由映射到运行时管理器（`src/console/ops_http_handler.cpp:805`）。Unix console dispatcher 在 bootstrap 构造期挂载 root 与 Lua 命令（`src/console/root_commands.cpp:25`，`src/console/lua_commands.cpp:24`）。浏览器应用是另一边界：它用会话级 bearer token 认证（`console/src/services/api.ts:14`），轮询具备取消、竞态保护和指数退避（`console/src/services/polling.ts:30`），页面只渲染快照（`console/src/pages/Dashboard.tsx:1`）。

## 拆分理由

1. **bootstrap 是唯一有序组合点。** 失败路径逐项清理每个已启动子系统，因为所有权和回滚在 bootstrap，不在各子系统（`src/bootstrap/bootstrap.cpp:306`）。
2. **配置先于消费者。** 可选模块分区在模块启动前被拒绝，避免 YAML 被静默忽略（`src/config/config.cpp:1115`）。
3. **CAF 消息契约隔离 Lua 与调用方。** 服务只收到类型化 `ServiceMessage`、`ClientIngress`、生命周期与响应值；网络线程和插件都不能直接操作 Lua state（`include/shield/core/service_message.hpp:46`，`src/lua/lua_gateway_bridge.cpp:120`）。
4. **一个 VM 线程拥有一个服务。** spawn、dispatch、协程挂起和强移都经 service manager 串行化，避免并发修改 Lua state（`src/lua/lua_service.cpp:2884`，`src/lua/lua_service.cpp:3495`）。
5. **网络准入与 Lua 路由在 session binding 处分离。** session 持有 target/player/epoch，gateway actor 单独应用改绑（`include/shield/net/session.hpp:107`，`include/shield/lua/gateway_actor.hpp:7`）。
6. **路由策略 header 先行。** envelope 解析和 descriptor 生成的路由表先于 ingress；bridge 在解码业务身份前拒绝未知、方向错误和未认证路由（`src/transport/protocol.cpp:1533`，`src/lua/lua_gateway_bridge.cpp:85`）。
7. **插件是具名 ABI 实例，不是链接进 host 的模块。** ABI/version/package 身份和 manifest schema 在 `create` 前校验，host 访问仅限显式依赖（`include/shield/plugin/abi.h:33`，`src/plugin/plugin_host.cpp:289`）。
8. **provider 拥有资源池。** Redis 包使用共享 driver 或显式内部 fallback；MySQL/PostgreSQL/SQLite/MongoDB 各自拥有 worker、事务和 pool drain 语义（`plugins/cache.redis/shield_cache_redis.cpp:969`，`plugins/mysql/shield_db_mysql.cpp:2042`）。
9. **全局状态按域拆分，不是一个大对象。** data、lock、rank 各自选择 backend，而 queue/scheduler/rate limit 留在进程内（`src/global/global_manager.cpp:319`，`src/global/global_manager.cpp:1196`）。
10. **跨节点传输与路由所有权分离。** discovery 只提供地址，transport 承载认证快照/envelope，`ClusterManager` 持有 liveness/routes（`include/shield/cluster/node_discovery.hpp:31`，`include/shield/cluster/cluster_messages.hpp:80`）。
11. **玩家身份是索引，不是 actor。** 它决定准入、重连和重复登录，然后指向玩家 service actor；业务状态留在该服务中（`src/player/player_manager.cpp:102`，`src/player/player_manager.cpp:229`）。
12. **服务器状态只有一个合法迁移面。** 静态迁移矩阵、watchers 和注入 stop callback 让关停协调不进入 Lua 或无关模块（`src/server/server_manager.cpp:60`）。
13. **可观测默认只读。** HTTP/console 暴露快照；eval/profile 需要单独 opt-in 和 token（`src/console/ops_http_handler.cpp:921`，`src/console/ops_http_handler.cpp:942`）。
14. **管理端只信任已认证 envelope。** bearer token 存在 `sessionStorage`，401 单独处理，且不会用旧响应覆盖新轮询（`console/src/services/api.ts:14`，`console/src/services/polling.ts:30`）。
15. **构建边界保持相同语义。** core/transport/net 是独立静态库；可选模块按条件编译；插件是运行时共享库而非 host 链接依赖（`CMakeLists.txt:345`，`CMakeLists.txt:424`，`CMakeLists.txt:506`）。

## 相关文档

- [源码分层术语表](glossary.md)
- [Source Layer Analysis](/source-layer-analysis)
- [核心设计理念](architecture-core-concepts.md)
- [Core Design Principles](/architecture-core-concepts)
