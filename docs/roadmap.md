# 重构路线图

Shield 仍处于重构设计阶段。旧文档中“Phase 1-7 全部完成”的描述不再作为当前口径。

说明：以下勾选只表示**文档边界和目标契约**已经冻结，不表示源码实现已经完成。

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

- [ ] 保留 `actor`、`net`、`transport`、`script`、`timer`、`data`、`config`、`log`。
- [ ] 将当前 `protocol/` 逐文件分类：字节流 framing/codec/encryption 进入 `shield_transport`，客户端 session 相关能力进入 `shield_net`，schema protocol 进入 deferred tooling。
- [ ] 处理 `gateway/`：gateway 是 Lua service pattern；当前 C++ gateway 代码只能迁移到 `shield_net`、模板示例或 legacy 删除路径。
- [ ] 将当前 `database/` 和 `data/` 收敛为一个 raw `shield_data` 模块。
- [ ] 删除或移出 `discovery`、`metrics`、`health`、`di`、`annotations`、`conditions`、`events`、`plugin`。
- [x] 决定物理目录策略：短期保留当前目录，用 CMake target 和 include 边界先收敛；不在 Phase 1 做大规模机械搬迁。

## Phase 2: Lua API 契约

- [x] 实现 Lua service module table loader 和 `on_init(args)` 最小调用路径。
- [x] 从 YAML `actors` 配置启动 Lua service 实例。
- [x] 实现 `shield.spawn` / `shield.exit` 的单节点最小路径。
- [x] 实现 opaque ServiceHandle、name reserve/publish 状态和 coroutine-aware spawn。
- [x] 实现 `shield.query/register/unregister/names` 的单节点最小 registry 路径。
- [x] 提供 `shield.now`；`timer_once/timer/sleep/fork` 仍是临时同步/线程实现。
- [x] 提供 `shield.log.*`。
- [x] 提供原始 `shield.db.*` / `shield.redis.*` 的绑定、未启用错误返回和启用 mock pool smoke test；真实后端连接仍按 `shield_data` 后续推进。
- [ ] 实现 `shield.call` 挂起当前 Lua 协程但不阻塞 runtime 线程的语义，并补齐默认超时和 `shield.call_timeout`。
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

- [x] 让 `examples/hello_world/` 接入统一 `shield::run` 入口并可构建启动。
- [x] 补齐 `examples/hello_world/` 的 Lua 业务消息验收。
- [x] 增加最小 Lua API runtime smoke test。
- [x] 增加本地 registry runtime smoke test。
- [x] 按 LAPI 矩阵补齐完整 Lua API 绑定测试。
- [x] 按 `docs/lua-api-tests.md` 补齐独立 API 用例，示例不替代测试。
- [x] 为新 public/core 头增加 CAF 泄漏静态检查。
- [x] 收敛 legacy public headers 的 CAF 泄漏并纳入检查。
- [x] 为 `shield_core` forbidden module include 增加静态检查。

## Phase 5: 官方可选模块

以下内容不属于当前 refactor core，但属于官方可选模块或扩展方向，可以在最小 runtime 稳定后推进：

- `shield_cluster`：多进程/多机器通信、节点心跳、远端路由 cache、可选服务发现。
- `shield_global`：跨进程数据、分布式锁、排行榜、队列、限流器。
- `shield_ops`：Prometheus 指标、健康检查、HTTP/console 管理端点、profile。
- [x] 冻结每个 optional module 的初始化失败策略：默认 fail fast；`shield_cluster` 允许远端连接失败时退化为单节点 unhealthy；未启用却配置 optional 段必须启动失败。

## Later

以下内容不属于当前 refactor core，也不属于已冻结的官方模块契约：

- 插件系统。
- 高级数据 mapper。
- Schema 工具链。
