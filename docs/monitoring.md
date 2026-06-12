# 可观测性

Prometheus、HealthCheckRegistry 和内置管理端点不属于当前重构 core。它们可以归入 `shield_ops` 官方可选模块。

## 当前保留

当前产品设计只保留日志模块；日志不是 `shield_core` 语义的一部分：

- 控制台日志。
- 文件日志作为 `shield_log` 的可选 sink 保留，默认关闭。
- Lua 目标 API：`shield.log.info/warn/error/debug`。

## Core 不提供

- Prometheus metrics。
- HealthCheckRegistry。
- `/health`、`/status`、`/metrics` HTTP 端点。
- 运行时诊断 HTTP API。
- 配置热重载监控器作为默认启动路径。

## 后续可选方向

运维能力作为官方可选模块或独立扩展实现：

- `shield_ops`：健康检查、诊断、控制台。
- `shield_ops` metrics exporter：Prometheus 格式指标。
- sidecar：由外部进程做健康检查和指标采集。

这些扩展不能反向污染 runtime core。
