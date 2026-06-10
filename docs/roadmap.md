# 重构路线图

Shield 仍处于重构设计阶段。旧文档中“Phase 1-7 全部完成”的描述不再作为当前口径。

## Phase 0: 文档和边界冻结

- [ ] 统一所有文档为单节点 Lua-first 运行时口径。
- [ ] 明确 core 非目标：discovery、metrics、health、plugin、DI/IoC、annotations、conditions、events、middleware chain、ORM。
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
- [ ] 明确 `shield.call` 是否阻塞 Lua 协程，以及超时语义。

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

## Later

以下内容不属于当前 refactor core，可作为独立扩展或用户方案重新评估：

- 多节点服务发现。
- Prometheus 指标。
- 健康检查注册表。
- HTTP 管理端点。
- 插件系统。
- 高级数据 mapper。
- Schema 工具链。
