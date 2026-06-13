# 重构路线图

Shield 仍处于重构设计阶段。旧文档中“Phase 1-7 全部完成”的描述不再作为当前口径。

说明：以下勾选表示对应源码路径已经进入当前 refactor 验证范围；文档边界冻结但源码仍未完成的事项保持未勾选。

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
- [ ] 实现 `timer_once/timer/sleep/fork` 的 coroutine-aware 语义；当前 `timer_once/timer/cancel_timer` 已走 `TimerManager`，但 `sleep`/`fork` 仍是阻塞/线程实现，callback 也不在协程中执行。
- [x] 提供 `shield.log.*`。
- [x] 提供原始 `shield.db.*` / `shield.redis.*` 的绑定和未启用错误返回。
- [ ] 补齐 data API 的真实 mock pool 验收和后端连接验证。`shield_runtime_data_smoke` 已覆盖 mock pool smoke；真实 MySQL/Redis 后端连接与连接池压力验证仍待补齐。
- [ ] 实现 `shield.call` 挂起当前 Lua 协程但不阻塞 runtime 线程的语义。`shield.call` / `shield.call_timeout` API 表面已注册并具备默认超时，但内部仍走同步 `LuaServiceManager::call` 路径，未挂起 Lua 协程。
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
- [ ] 按 LAPI 矩阵补齐完整 Lua API 绑定测试；LAPI-001~010 已在 `tests/lua_api/` 覆盖正负向 case，但 `shield.call` / `timer` / `sleep` / `fork` 的 coroutine-aware 路径仍依赖 Phase 2 实现落地后再补完。
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
| `shield_lua` | 当前 CMake target 已存在 | module table/on_init/spawn/registry/基础 API 已接入；coroutine-aware call、timer/task、gateway/data 完整测试仍待完成 |
| `shield_bootstrap` | 当前 CMake target 已存在 | `shield::run` 和 CLI/config smoke tests 已登记在主 CMake |
| optional modules | CMake 开关存在，默认关闭 | `shield_cluster/global/ops` 只有占位 target，未进入实现完成范围 |

## Phase 5: 官方可选模块

以下内容不属于当前 refactor core，但属于官方可选模块或扩展方向，可以在最小 runtime 稳定后推进：

- `shield_cluster`：多进程/多机器通信、节点心跳、远端路由 cache、可选服务发现。
- `shield_global`：跨进程数据、分布式锁、排行榜、队列、限流器。
- `shield_ops`：Prometheus 指标、健康检查、HTTP/console 管理端点、profile。
- [x] 冻结每个 optional module 的初始化失败策略：默认 fail fast；`shield_cluster` 允许远端连接失败时退化为单节点 unhealthy；未启用却配置 optional 段必须启动失败。
- [x] 冻结 `shield_player` 文档契约：`shield.player.setup` 主 API 与默认 hook 实现表、persistence adapter 边界、`PlayerRef` 本地/远端边界、anonymous/spectator opt-in 状态、多设备策略、`player_pool` 容量模型和 `shield.player.Base` 语法糖边界。实现仍按 P0/P1/P2 分阶段推进。

## Later

以下内容不属于当前 refactor core，也不属于已冻结的官方模块契约：

- 插件系统。
- 高级数据 mapper。
- Schema 工具链。
