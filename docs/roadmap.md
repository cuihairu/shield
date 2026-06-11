# 重构路线图

Shield 仍处于重构设计阶段。旧文档中“Phase 1-7 全部完成”的描述不再作为当前口径。

## Phase 0: 文档和边界冻结

- [ ] 统一所有文档为单节点优先、Lua-first 运行时口径。
- [ ] 明确 core 非目标：discovery、metrics、health、plugin、DI/IoC、annotations、conditions、events、middleware chain、ORM。
- [ ] 明确 `shield_cluster` 是官方可选模块和后续阶段，不进入 `shield_core`，但保留地址、超时和错误语义。
- [ ] 区分目标 API、当前实现、旧架构遗留模块。

## Phase 1: 运行时目录重排

- [ ] 保留 `actor`、`net`、`transport`、`script`、`timer`、`data`、`config`、`log`。
- [ ] 将 `protocol/` 合并到 `net/` 或 `transport/`。
- [ ] 处理 `gateway/`：保留为 Lua 模板，或收敛为 `net` 上的薄适配。
- [ ] 删除或移出 `discovery`、`metrics`、`health`、`di`、`annotations`、`conditions`、`events`、`plugin`。

## Phase 2: Lua API 契约

- [ ] 实现 `shield.service`。
- [ ] 实现 `shield.spawn` / `shield.exit`。
- [ ] 统一 `shield.timer` / `shield.timer_once` / `shield.now`。
- [ ] 提供 `shield.log.*`。
- [ ] 提供原始 `shield.db.*` / `shield.redis.*`。
- [ ] 实现 `shield.call` 挂起当前 Lua 协程但不阻塞 runtime 线程的语义，并补齐默认超时和 `shield.call_timeout`。

## Phase 3: C++ 入口和配置

- [ ] 增加 `include/shield/shield.hpp`。
- [ ] 实现 `shield::run(argc, argv)`。
- [ ] 明确 YAML `actors`、`network`、`database`、`redis`、`log` 的最小 schema。
- [ ] 移除旧 CLI 文档与新入口冲突。

## Phase 4: 示例和测试

- [ ] 让 `examples/hello_world/` 成为可运行验收示例。
- [ ] 为 Lua API 增加绑定测试。
- [ ] 为模块边界增加 CI 检查。
- [ ] 为 forbidden module include 增加静态检查。

## Phase 5: 官方可选模块

以下内容不属于当前 refactor core，但属于官方可选模块或后续阶段，可以在最小 runtime 稳定后推进：

- `shield_cluster`：多进程/多机器通信、节点心跳、远端路由 cache、可选服务发现。
- `shield_global`：跨进程数据、分布式锁、排行榜、队列、限流器。
- `shield_ops`：Prometheus 指标、健康检查、HTTP/console 管理端点、profile。

## Later

以下内容不属于当前 refactor core，也不属于已冻结的官方模块契约：

- 插件系统。
- 高级数据 mapper。
- Schema 工具链。
