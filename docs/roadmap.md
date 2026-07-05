# 重构路线图

Shield 仍处于重构设计阶段。旧文档中“Phase 1-7 全部完成”的描述不再作为当前口径。

说明：以下勾选表示对应源码路径已经进入当前 refactor 验证范围；文档边界冻结但源码仍未完成的事项保持未勾选。

当前收敛目标是先关闭一个可验证的单节点最小闭环：`shield::run`、Phase 1 配置验证、Lua module-table service、local registry、基础 `send/call` 返回形态、TCP gateway 边界、插件系统 v1 的 manifest/instance/binding 主路径和缺失插件能力的明确错误。handler 内 coroutine-aware `call/sleep` 已进入当前实现路径；任何需要多节点、真实后端、timer/fork coroutine 化、UDP/KCP/WebSocket 或官方可选模块的能力都不能作为当前 Phase 1 阻塞项。

设计决策记录集中在 [Decision Log](open-decisions.md)。如果后续新增开放问题，需要先记录，再同步更新对应权威文档和本路线图。

## Phase 0: 文档和边界冻结

- [x] 统一所有文档为单节点优先、Lua-first 运行时口径。
- [x] 明确 core 非目标：discovery、metrics、health、plugin、DI/IoC、annotations、conditions、events、middleware chain、重 ORM / XML mapper codegen。
- [x] 明确 `shield_cluster` 是官方可选模块，不进入 `shield_core`，但保留地址、超时和错误语义。
- [x] 区分目标 API、当前实现、旧架构遗留模块。
- [x] 以 `docs/lua-api.md` 冻结 Lua API 契约。
- [x] 以 `docs/lua-api-tests.md` 冻结 Lua API 测试矩阵。
- [x] 以 `docs/cmake-refactor.md` 冻结 CMake target 拆分方向。

## Phase 1: 运行时目录重排

- [x] 保留 `actor`、`net`、`transport`、`script`、`timer`、`config`、`log`、`plugin`。语义归属：actor 由 `shield_core` 承载，script/timer 由 `shield_lua` + `shield_core` 协作，后端能力由 `shield_plugin` 和独立插件承载。
- [x] 将当前 `protocol/` 逐文件分类：字节流 framing/codec/encryption 进入 `shield_transport`，客户端 session 相关能力进入 `shield_net`，schema protocol 进入 deferred tooling。`shield/protocol/*` 与 `shield/transport/protocol/*` 残留 header 与对应源文件、测试已在本次清理中删除。
- [x] 处理 `gateway/`：gateway 收敛为 Lua service pattern；C++ gateway 残留代码与对应测试已删除。
- [x] 废弃 raw `shield_data` 模块方向；数据访问收敛为插件系统 v1 + `include/shield/plugin/*` C ABI + 独立 provider 插件。
- [x] 删除或移出 `discovery`、旧 core `metrics`、旧 core `health`、`di`、`annotations`、`conditions`、`events`、旧插件 v0。对应源码、headers 与测试已一并清除；`shield_extensions` target 不再存在。
- [x] 决定物理目录策略：短期保留当前目录，用 CMake target 和 include 边界先收敛；不在 Phase 1 做大规模机械搬迁。

## Phase 2: Lua API 契约

- [x] 实现 Lua service module table loader 和 `on_init(args)` 最小调用路径。
- [x] 从 `actors` 配置启动 Lua service 实例。
- [x] 实现 `shield.spawn` / `shield.exit` 的单节点最小路径。
- [x] 实现 opaque ServiceHandle、name reserve/publish 状态和 coroutine-aware spawn。
- [x] 实现 `shield.query/register/unregister/names` 的单节点最小 registry 路径。
- [x] 提供 `shield.now`。
- [x] 实现 `timer_once/timer/cancel_timer`、handler 内 coroutine-aware `sleep`、`fork` task id。
  - `timer_once/timer/cancel_timer` 已走 `TimerManager`，callback 通过 `check_and_fire_each` + visitor 中 `lua_pcall` 包裹执行，错误路由到 `on_error` hook。
  - `fork` 已走 worker 线程调度（`enqueue_forked_task`），callback 通过 `lua_pcall` 包裹执行（`raw_fn` 有效时），错误路由到 `on_error` hook。
  - `shield.sleep` 已实现 handler 协程感知：async handler 中 yield + 由 `_resume_after` 定时器 resume；sync 调用、timer callback 和 fork task 路径走 `_block_sleep` 阻塞降级。LAPI-007-08/09/10 已覆盖。
  - `shield.call` / `shield.call_timeout` 已实现协程感知调用路径（`_coro_call` → `suspend_for_call` + `coroutine.yield()` → mailbox → `call_service_method_coroutine` → `resume_caller`），主线程走 `_sync_call` 同步降级。call timeout 已通过 `pump_once` 中的 `check_call_timeouts` 实现。LAPI-005-06 已覆盖。
- [x] 提供 `shield.log.*`。
- [x] 删除旧 `shield.db.*` / `shield.redis.*` 全局数据 API，数据访问统一走插件 namespace + binding 逻辑名。
- [x] 插件系统 v1 提供 manifest scan、catalog、instance、binding、C ABI guard、`register_lua` 分发和只读 introspection。
- [x] 迁移官方数据插件的 Lua callable namespace：`shield.database.*` / `shield.cache.redis` / `shield.queue.redis` / `shield.leaderboard.redis` 均接收 binding 而非 instance_id。
- [x] Lua 层 mapper/helper 只能作为插件 proxy 上的辅助能力存在；XML schema-mapper parser / descriptor / codegen 仍属 deferred 设计，不进入当前最小 runtime。
- [x] 实现 `shield.call` 挂起当前 Lua 协程但不阻塞 runtime 线程的语义。`shield.call` / `shield.call_timeout` 已实现协程感知调用路径（`_coro_call` → `suspend_for_call` → `coroutine.yield()` → `resume_caller`），call timeout 已通过 `check_call_timeouts` 实现。LAPI-005-06 已覆盖协程 timeout 和同步降级两条路径。
- [x] 删除旧 `shield.service("name")`、冒号式 DB/Redis API 和 legacy `on_message(src, type, data)` 入口。

## Phase 3: C++ 入口和配置

- [x] 增加 `include/shield/shield.hpp`。
- [x] 实现顶层 `shield::run(argc, argv)`，包装或替代当前 `shield::bootstrap::run(argc, argv)`。
- [x] 冻结 CLI 细节：`--config` 默认值/必填、多配置文件、`--node-id` 归属、legacy subcommand 去留、退出码。
- [x] 明确 `actors`、`network`、`plugins`、`log` 的最小 schema。
- [x] 实现启动期最小配置验证：optional 配置段未启用即失败、Phase 1 deferred transport 拒绝启动、actor/plugin 基础字段校验。
- [x] 为 `shield` 可执行文件增加 CLI/config smoke tests。
- [x] 移除旧 CLI 文档与新入口冲突。

## Phase 4: 示例和测试

- [x] 让 `examples/hello_world/` 接入统一 `shield::run` 入口。
- [x] 验证 `examples/hello_world/` 可构建启动。构建由 `examples/CMakeLists.txt` 注册；启动验收由 `test_hello_world_acceptance` 在构建产物上检查 `shield::run` 入口、配置文件与 Lua 脚本契约。运行时端到端 smoke 由 `shield_runtime_lua_smoke` / `shield_runtime_registry_smoke` / `shield_runtime_data_smoke` 在 `shield --check-config` 路径上覆盖。
- [x] 补齐 `examples/hello_world/` 的 Lua 业务消息验收。acceptance test 已覆盖 `echo.lua` 的 sender/send/log/now、`gateway.lua` 的 connect/disconnect/client_message、`player.lua` 的 login/chat/logout/self/exit 和插件 namespace API 模式。
- [x] 增加最小 Lua API runtime smoke test。
- [x] 增加本地 registry runtime smoke test。
- [x] 按 LAPI 矩阵补齐当前 Lua API 绑定测试；当前 `tests/lua_api/` 覆盖 lifecycle、timers、registry、messaging、call、context、legacy、config 和 gateway。插件 Lua API 由插件系统和各 provider 测试继续补齐。
- [x] 按 `docs/lua-api-tests.md` 补齐独立 API 用例，示例不替代测试。
- [x] 为新 public/core 头增加 CAF 泄漏静态检查。
- [x] 收敛 legacy public headers 的 CAF 泄漏并纳入检查。
- [x] 为 `shield_core` forbidden module include 增加静态检查。

## 当前模块核对快照

| 模块 | 当前状态 | 核对结论 |
| --- | --- | --- |
| `shield_base` | 当前 CMake target 已存在 | 基础类型路径已收敛，仍需后续补完整单元测试 |
| `shield_core` | 当前 CMake target 已存在 | registry/message/handle 边界基本符合文档，CAF 仅允许在 adapter 边界 |
| `shield_config` | 当前 CMake target 已存在 | Phase 1 启动期验证已接入；旧 ConfigManager/动态配置测试已从当前构建入口移出 |
| `shield_log` | 当前 CMake target 已存在 | logger facade 已接入；旧 Boost log config 测试已从当前构建入口移出 |
| `shield_transport` | 当前 CMake target 已存在 | frame/codec/encryption 在 target 内；旧 protocol handler/schema protocol 测试不属于当前验证路径 |
| `shield_net` | 当前 CMake target 已存在 | 单实例 TCP listener/session 已接入 bootstrap 的 `actors[].network.tcp`；UDP/WebSocket 仍为 deferred |
| `shield_plugin` | 当前 CMake target 已存在 | 插件系统 v1 已接入 manifest、instance、binding、C ABI、Lua register_lua 和官方数据插件；host 不链接 DB/Redis 驱动，不存在 `shield_data` target |
| `shield_lua` | 当前 CMake target 已存在 | module table/on_init/spawn/registry/基础 API 已接入；coroutine-aware sleep/call/timeout 已实现；timer callback 已通过 pcall 包裹执行；fork callback raw_fn 已存储；on_error/on_panic/on_exit guard 已实现；gateway 已通过真实 `SessionHandle` userdata 连接到 `shield_net::Session`，并覆盖 protocol ingress/egress 桥接测试 |
| `shield_bootstrap` | 当前 CMake target 已存在 | `shield::run` 和 CLI/config smoke tests 已登记在主 CMake |
| optional modules | CMake 开关存在，默认关闭 | `shield_cluster` 已有静态 peers、节点状态快照和 route cache 查询骨架；跨节点 transport/route 学习仍未实现。`shield_global/ops` 仍未进入实现完成范围 |

## Phase 5: 官方可选模块

以下内容不属于当前 refactor core，但属于官方可选模块或扩展方向，可以在最小 runtime 稳定后推进：

- `shield_cluster`：多进程/多机器通信、节点心跳、远端路由 cache、可选服务发现。
- `shield_global`：跨进程数据、分布式锁、排行榜、队列、限流器。
- `shield_player`：玩家 setup、PlayerRef、PlayerManager、重连、持久化 adapter。
- `shield_server`：服务器状态、维护模式、关闭广播、运行时信息。
- `shield_ops`：Prometheus 指标、健康检查、HTTP/console 管理端点、profile。
- [x] 冻结每个 optional module 的初始化失败策略：默认 fail fast；`shield_cluster` 允许远端连接失败时退化为单节点 unhealthy；未启用却配置 optional 段必须启动失败。
- [x] 冻结 `shield_player` 文档契约：`shield.player.setup` 主 API 与默认 hook 实现表、persistence adapter 边界、`PlayerRef` 本地/远端边界、anonymous/spectator opt-in 状态、多设备策略、`player_pool` 容量模型和 `shield.player.Base` 语法糖边界。实现仍按 P0/P1/P2 分阶段推进。

## 插件系统

详见 [插件系统设计文档](plugin-system.md)。

当前插件系统按 v1 重新设计，不兼容旧实验实现。旧的 `PluginManager`、`plugins.enabled`、`shield_plugin_api()`、`shield_plugin_type`、全局 `find_plugin` 和 `shield_redis` 基础设施插件口径不再作为完成状态。

- [x] 冻结 v1 文档方向：metadata-first、外部 YAML manifest、发现/加载/启动分离、package/instance/binding 模型。
- [x] 实现 manifest 扫描和 catalog 构建。
- [x] 实现 `plugins.instances`、`plugins.bindings` 和依赖解析。
- [x] 实现 `shield_plugin_get_v1()` ABI 入口和二进制 guard。
- [x] 实现配置 schema 校验和默认值合并。
- [x] 实现结构化错误对象和加载阶段错误报告。
- [x] 实现 Lua introspection：`packages`、`instances`、`instance`、`binding`。
- [x] 迁移 DATABASE / CACHE / QUEUE / LEADERBOARD / AUTH / METRIC / HEALTH / MATCHMAKING 等 provider 到 interface-based v1（含 sqlite/mysql/postgresql/mongodb/redis 系/queue/leaderboard/auth.jwt/metrics.prometheus/health.http/matchmaking.elo）。
- [x] 决策：v1 不支持热加载、沙箱、请求级生命周期；这些方向不进入 v1，未来如需引入须重新设计 ABI 版本。

## Later

以下内容不属于当前 refactor core，也不属于已冻结的官方模块契约：

- 高级数据 mapper。
- Schema 工具链。
- Lua runtime primitives / cosocket 风格出站 I/O（如 `shield.buffer` / `shield.crypto` / `shield.socket` / `shield.stream` / `shield.tls`）。

说明：

- 当前主路径不要求用户理解或使用上述 primitives。
- 当前推荐心智模型仍然是：客户端入站走 gateway / `SessionHandle`，服务间走 `shield.send/call`，对外 HTTP 走 `shield.http`。
- primitives / cosocket 方向只保留为后置草案，不作为当前阶段阻塞项。
