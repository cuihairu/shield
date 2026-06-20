# 重构路线图

Shield 仍处于重构设计阶段。旧文档中“Phase 1-7 全部完成”的描述不再作为当前口径。

说明：以下勾选表示对应源码路径已经进入当前 refactor 验证范围；文档边界冻结但源码仍未完成的事项保持未勾选。

当前收敛目标是先关闭一个可验证的单节点最小闭环：`shield::run`、Phase 1 配置验证、Lua module-table service、local registry、基础 `send/call` 返回形态、TCP gateway 边界、DB/Redis 未启用错误和 mock data smoke。任何需要多节点、真实后端、完整 coroutine 调度、UDP/KCP/WebSocket 或官方可选模块的能力都不能作为当前 Phase 1 阻塞项。

设计决策记录集中在 [Decision Log](open-decisions.md)。如果后续新增开放问题，需要先记录，再同步更新对应权威文档和本路线图。

## Phase 0: 文档和边界冻结

- [x] 统一所有文档为单节点优先、Lua-first 运行时口径。
- [x] 明确 core 非目标：discovery、metrics、health、plugin、DI/IoC、annotations、conditions、events、middleware chain、ORM。
- [x] 明确 `shield_cluster` 是官方可选模块，不进入 `shield_core`，但保留地址、超时和错误语义。
- [x] 区分目标 API、当前实现、旧架构遗留模块。
- [x] 以 `docs/lua-api.md` 冻结 Lua API 契约。
- [x] 以 `docs/lua-api-tests.md` 冻结 Lua API 测试矩阵。
- [x] 以 `docs/cmake-refactor.md` 冻结 CMake target 拆分方向。

## Phase 1: 运行时目录重排

- [x] 保留 `actor`、`net`、`transport`、`script`、`timer`、`data`、`config`、`log`。语义归属：actor 由 `shield_core` 承载，script/timer 由 `shield_lua` + `shield_core` 协作，其余按 target 边界落入对应模块。
- [x] 将当前 `protocol/` 逐文件分类：字节流 framing/codec/encryption 进入 `shield_transport`，客户端 session 相关能力进入 `shield_net`，schema protocol 进入 deferred tooling。`shield/protocol/*` 与 `shield/transport/protocol/*` 残留 header 与对应源文件、测试已在本次清理中删除。
- [x] 处理 `gateway/`：gateway 收敛为 Lua service pattern；C++ gateway 残留代码与对应测试已删除。
- [x] 将当前 `database/` 和 `data/` 收敛为一个 raw `shield_data` 模块。
- [x] 删除或移出 `discovery`、`metrics`、`health`、`di`、`annotations`、`conditions`、`events`、`plugin`。对应源码、headers 与测试已一并清除；`shield_extensions` target 不再存在。
- [x] 决定物理目录策略：短期保留当前目录，用 CMake target 和 include 边界先收敛；不在 Phase 1 做大规模机械搬迁。

## Phase 2: Lua API 契约

- [x] 实现 Lua service module table loader 和 `on_init(args)` 最小调用路径。
- [x] 从 YAML `actors` 配置启动 Lua service 实例。
- [x] 实现 `shield.spawn` / `shield.exit` 的单节点最小路径。
- [x] 实现 opaque ServiceHandle、name reserve/publish 状态和 coroutine-aware spawn。
- [x] 实现 `shield.query/register/unregister/names` 的单节点最小 registry 路径。
- [x] 提供 `shield.now`。
- [x] 实现 `timer_once/timer/sleep/fork` 的 coroutine-aware 语义。
  - `timer_once/timer/cancel_timer` 已走 `TimerManager`，callback 通过 `check_and_fire_each` + visitor 中 `lua_pcall` 包裹执行，错误路由到 `on_error` hook。
  - `fork` 已走 worker 线程调度（`enqueue_forked_task`），callback 通过 `lua_pcall` 包裹执行（`raw_fn` 有效时），错误路由到 `on_error` hook。
  - `shield.sleep` 已实现协程感知：async handler 中 yield + 由 `_resume_after` 定时器 resume；sync 调用路径走 `_block_sleep` 阻塞降级。LAPI-007-08 已覆盖。
  - `shield.call` / `shield.call_timeout` 已实现协程感知调用路径（`_coro_call` → `suspend_for_call` + `coroutine.yield()` → mailbox → `call_service_method_coroutine` → `resume_caller`），主线程走 `_sync_call` 同步降级。call timeout 已通过 `pump_once` 中的 `check_call_timeouts` 实现。LAPI-005-06 已覆盖。
- [x] 提供 `shield.log.*`。
- [x] 提供原始 `shield.db.*` / `shield.redis.*` 的绑定和未启用错误返回。
- [x] 补齐 data API 的真实 mock pool 验收和后端连接验证。`shield_runtime_data_smoke` 已覆盖 mock pool smoke；`tests/lua_api/test_lua_api_data.cpp` 已覆盖 mock pool 下的 DB query/execute、Redis get/set/del/subscribe/exists 和 dot-notation 负向测试（10 个用例）。真实 MySQL/Redis 后端连接与连接池压力验证属于 Phase 2+，不阻塞 Phase 1 闭环。
- [x] 实现 `shield.call` 挂起当前 Lua 协程但不阻塞 runtime 线程的语义。`shield.call` / `shield.call_timeout` 已实现协程感知调用路径（`_coro_call` → `suspend_for_call` → `coroutine.yield()` → `resume_caller`），call timeout 已通过 `check_call_timeouts` 实现。LAPI-005-06 已覆盖协程 timeout 和同步降级两条路径。
- [x] 删除旧 `shield.service("name")`、冒号式 DB/Redis API 和 legacy `on_message(src, type, data)` 入口。

## Phase 3: C++ 入口和配置

- [x] 增加 `include/shield/shield.hpp`。
- [x] 实现顶层 `shield::run(argc, argv)`，包装或替代当前 `shield::bootstrap::run(argc, argv)`。
- [x] 冻结 CLI 细节：`--config` 默认值/必填、多配置文件、`--node-id` 归属、legacy subcommand 去留、退出码。
- [x] 明确 YAML `actors`、`network`、`database`、`redis`、`log` 的最小 schema。
- [x] 实现启动期最小配置验证：optional 配置段未启用即失败、Phase 1 deferred transport 拒绝启动、actor/data 基础字段校验。
- [x] 为 `shield` 可执行文件增加 CLI/config smoke tests。
- [x] 移除旧 CLI 文档与新入口冲突。

## Phase 4: 示例和测试

- [x] 让 `examples/hello_world/` 接入统一 `shield::run` 入口。
- [x] 验证 `examples/hello_world/` 可构建启动。构建由 `examples/CMakeLists.txt` 注册；启动验收由 `test_hello_world_acceptance` 在构建产物上检查 `shield::run` 入口、配置文件与 Lua 脚本契约。运行时端到端 smoke 由 `shield_runtime_lua_smoke` / `shield_runtime_registry_smoke` / `shield_runtime_data_smoke` 在 `shield --check-config` 路径上覆盖。
- [x] 补齐 `examples/hello_world/` 的 Lua 业务消息验收。acceptance test 已覆盖 `echo.lua` 的 sender/send/log/now、`gateway.lua` 的 connect/disconnect/client_message、`player.lua` 的 login/chat/logout/self/exit/db/redis API 模式。
- [x] 增加最小 Lua API runtime smoke test。
- [x] 增加本地 registry runtime smoke test。
- [x] 按 LAPI 矩阵补齐完整 Lua API 绑定测试；当前 `tests/lua_api/` 已启用 lifecycle（15）、timers（8）、registry（8）、messaging（6）、call（9）、context（5）、legacy（5）、data（10）、gateway（6）共 9 个套件 72 个用例，全部通过。data 套件覆盖 mock pool 下 DB/Redis API；gateway 套件覆盖 connect/message/disconnect/queue_full/stale_send 模拟；coroutine-aware call/sleep/timer/fork 均已实现并有测试覆盖。
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
| `shield_net` | 当前 CMake target 已存在 | TCP listener/session 在 target 内；HTTP/UDP/WebSocket 源码属于 deferred/legacy，不进入 Phase 1 config |
| `shield_data` | 当前 CMake target 已存在 | raw DB/Redis facade 已接入；真实后端和 mock pool 验收仍待补齐 |
| `shield_lua` | 当前 CMake target 已存在 | module table/on_init/spawn/registry/基础 API 已接入；coroutine-aware sleep/call/timeout 已实现；timer callback 已通过 pcall 包裹执行；fork callback raw_fn 已存储；on_error/on_panic/on_exit guard 已实现；gateway session 测试仍待 mock harness |
| `shield_bootstrap` | 当前 CMake target 已存在 | `shield::run` 和 CLI/config smoke tests 已登记在主 CMake |
| optional modules | CMake 开关存在，默认关闭 | `shield_cluster/global/ops` 只有占位 target，未进入实现完成范围 |

## Phase 5: 官方可选模块

以下内容不属于当前 refactor core，但属于官方可选模块或扩展方向，可以在最小 runtime 稳定后推进：

- `shield_cluster`：多进程/多机器通信、节点心跳、远端路由 cache、可选服务发现。
- `shield_global`：跨进程数据、分布式锁、排行榜、队列、限流器。
- `shield_player`：玩家 setup、PlayerRef、PlayerManager、重连、持久化 adapter。
- `shield_server`：服务器状态、维护模式、关闭广播、运行时信息。
- `shield_ops`：Prometheus 指标、健康检查、HTTP/console 管理端点、profile。
- [x] 冻结每个 optional module 的初始化失败策略：默认 fail fast；`shield_cluster` 允许远端连接失败时退化为单节点 unhealthy；未启用却配置 optional 段必须启动失败。
- [x] 冻结 `shield_player` 文档契约：`shield.player.setup` 主 API 与默认 hook 实现表、persistence adapter 边界、`PlayerRef` 本地/远端边界、anonymous/spectator opt-in 状态、多设备策略、`player_pool` 容量模型和 `shield.player.Base` 语法糖边界。实现仍按 P0/P1/P2 分阶段推进。

## Later

以下内容不属于当前 refactor core，也不属于已冻结的官方模块契约：

- 插件系统。
- 高级数据 mapper。
- Schema 工具链。

## 审计发现与待处理项

以下为 2026-06-20 文档-代码对齐审计中发现的问题。

### 已修复

| 编号 | 问题 | 修复 |
| --- | --- | --- |
| GAP-001 | `test_lua_api_data.cpp` 使用不存在的 `manager->spawn_service()` API | 按当前 `manager.spawn()` 接口重写，覆盖 mock pool 下 DB/Redis API |
| GAP-002 | `test_lua_api_gateway.cpp` 使用不存在的 `manager->spawn_service()` API | 按当前接口重写，覆盖模块加载和 handler 存在性检查 |
| GAP-003 | `test_lua_api_data.cpp` / `test_lua_api_gateway.cpp` 在 CMakeLists.txt 中被注释 | 已启用；data 测试链接 `shield_data` |
| GAP-004 | `test_lua_api_lifecycle.cpp` LAPI-002-03 断言与注释矛盾 | 修正断言：`on_init` 返回 `false` 时 `call_service_function` 应返回 `false` |
| GAP-005 | `data_service.lua` 缺少 dot-notation 负向测试方法 | 新增 `test_colon_db_fails` |

### 待实现（需 Phase 2 coroutine 支持）

| 编号 | 问题 | 阻塞原因 |
| --- | --- | --- |
| GAP-010 | ~~`shield.call` 协程路径已实现，但 call timeout 未实现~~ 已实现：`check_call_timeouts` 已接入 `pump_once`，LAPI-005-06 覆盖 | 已完成 |
| GAP-011 | ~~`shield.sleep` 是阻塞实现~~ 已改为协程 yield/resume（async 派发路径非阻塞，sync 调用保留阻塞降级），LAPI-007-08 覆盖 | 已完成 |
| GAP-012 | ~~`shield.fork` 的 callback 不在协程中执行~~ 已实现：`raw_fn` 有效时通过 `lua_pcall` 包裹执行，错误路由到 `on_error` hook | 已完成 |
| GAP-013 | ~~timer callback 不在协程中执行~~ 已实现：`check_and_fire_each` + visitor 中 `lua_pcall` 包裹，错误路由到 `on_error` hook | 已完成 |
| GAP-014 | ~~LAPI-005-06 (call timeout) 测试~~ 已更新为 `CoroutineCallTimeout` + `SyncCallIgnoresTimeout`，覆盖两条路径 | 已完成 |
| GAP-015 | ~~`on_exit` 中调用 `shield.call` 返回 `api_not_allowed_in_exit`~~ 已实现：`_is_in_exit()` 检查 + Lua wrapper 返回 `{code="api_not_allowed_in_exit"}`，`OnExitCallGuard` 测试覆盖 | 已完成 |

### 待实现（需专用 mock harness）

| 编号 | 问题 | 阻塞原因 |
| --- | --- | --- |
| GAP-020 | ~~LAPI-009-01~05 (Gateway session 模拟测试)~~ 已覆盖：009-01 (connect)、009-02 (message)、009-03 (disconnect)、009-04 (queue full handled)、009-05 (stale send handled) 共 6 个 gateway 测试通过 | 已完成 |
| GAP-021 | ~~LAPI-008-03 (SQL error → `database_error`)~~ 已实现：`set_mock_db_error` 注入错误 + `LAPI_008_03_DbQueryReturnsError` / `LAPI_008_03b_DbExecuteReturnsError` 测试覆盖 | 已完成 |
| GAP-022 | ~~`on_error` / `on_panic` hook 注册与调用~~ 已实现：`LuaRuntime::invoke_hook` 调用 service table 上的 `on_error`/`on_panic`；handler 错误通过 `call_service_method_coroutine` 触发，timer 错误通过 `check_and_fire` 回调触发，fork 错误通过 `pump_once` 触发；连续错误达阈值（默认 10）触发 `on_panic` + `exit("panic")`。`OnErrorHookCalledOnHandlerThrow` 测试覆盖 | 已完成 |
| GAP-023 | ~~`shield.config` API 缺少独立测试~~ 已补充：`ConfigReadExistingKey`、`ConfigReadMissingKeyReturnsNil`、`ConfigReadMissingKeyWithDefault` 3 个测试覆盖 | 已完成 |

### 文档不合理项

| 编号 | 文档 | 问题 | 建议 |
| --- | --- | --- | --- |
| DOC-001 | lua-api.md | `shield.config("database.host", "localhost")` 示例暗示嵌套 key 访问，但实现只做扁平 key 匹配 | 补充说明 config key 是扁平字符串，不支持嵌套路径遍历 |
| DOC-002 | lua-api.md | `on_error` / `on_panic` 定义了 `context.type` 可取 `handler`/`timer`/`fork`，但 timer/fork callback 当前不走协程，无法自然产生 error context | 标注为 Phase 2 语义 |
| DOC-003 | lua-api.md | `shield.trace()` 和 `shield.deadline()` 当前返回固定值 `"trace:0"` 和 `nil` | 标注为 Phase 2 实现 |
| DOC-004 | lua-api-tests.md | LAPI-005-07 (late response after timeout) 和 LAPI-005-08 (nested call) 依赖 coroutine-aware call | 标注为 Phase 2 |
| DOC-005 | roadmap.md Phase 2 | `fork` 描述为"仍是线程实现"，实际已走 worker 线程调度（`enqueue_forked_task`），非 `std::thread::detach` | 已更新描述 |

### 2026-06-20 二次审核发现

| 编号 | 问题 | 状态 |
| --- | --- | --- |
| AUDIT-001 | roadmap.md `shield.call` 描述为"仍走同步路径"，实际已有 `_coro_call` + `suspend_for_call` + `resume_caller` 协程实现 | 已修正文档 |
| AUDIT-002 | ~~`pump_once` 缺少 `check_call_timeouts`~~ 已实现：`check_call_timeouts` 扫描 `pending_calls` 过期条目，以 `{code="timeout"}` 恢复 caller，LAPI-005-06 覆盖 | 已完成 |
| AUDIT-003 | ~~`test_lua_api_call.cpp` LAPI-005-06 测试名与代码不符~~ 已更新：新增 `LAPI_005_06_CoroutineCallTimeout` 覆盖协程 timeout 路径，原测试改名为 `LAPI_005_06_SyncCallIgnoresTimeout` | 已完成 |
| AUDIT-004 | `lua-api-tests.md` Phase 2 延迟用例表中 LAPI-007-05 标为延迟，但 LAPI-007-08 已覆盖 sleep 协程语义 | 已修正 |
| AUDIT-005 | `process_mailbox` 使用 `call_service_method_coroutine` 派发，handler 在协程中执行；但 `call` 的同步路径 `LuaServiceManager::call` 仍走 `call_service_method` 非协程路径 | 已知，同步降级设计如此 |
