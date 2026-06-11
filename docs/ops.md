# 运维与调试设计

`shield_ops` 是 Shield 的官方可选运维与调试层，不属于 `shield_core`，也不是最小运行路径。

它的目标不是给游戏业务提供新 API，而是让运行时在不侵入核心语义的前提下，可被观察、诊断、采样和控制。

## 设计目标

- 观察运行时内部状态，而不暴露 CAF 细节给业务脚本。
- 在开发、测试、预发环境提供足够的诊断信息。
- 在生产环境提供受控、低开销、可鉴权的运维入口。
- 所有 ops 能力都应可关闭，不影响 `shield_core` 启动。

## 典型能力

| 能力 | 说明 |
| --- | --- |
| Metrics | 计数器、直方图、gauge，用于吞吐、延迟、错误率 |
| Diagnostics | 服务列表、actor 数量、pending call、timer、Lua VM 池、连接数 |
| Console | 交互式调试控制台，查询、调用、列服务、看状态 |
| Profile | 采样式热点分析、消息延迟剖面、慢调用追踪 |
| Health | 进程存活、关键模块就绪、资源状态 |

## 适合暴露的内部状态

- 服务注册表和服务命名结果。
- 活跃服务数量、类型分布、每服务消息计数。
- pending call 数量和超时统计。
- timer 数量、定时器触发频率、取消统计。
- Lua VM 池使用率、回收率、加载失败数。
- 网络连接数、session 数、协议吞吐。
- IPC / cluster 节点 heartbeat 状态、离线时间、tombstone 数量。
- DB / Redis 连接池状态。
- 最近错误、最近慢请求、最近一次配置加载结果。

## 不适合暴露的内部状态

- CAF actor handle。
- 私有 message payload 的完整内容。
- 业务敏感字段原文。
- 不带鉴权的远程控制入口。
- 依赖运维侧才能理解的内部调试对象。

## 对外接口形态

`shield_ops` 可以通过以下形态暴露：

```text
HTTP debug endpoints
CLI console
local admin socket
internal metrics exporter
sampling profiler
```

建议端点按能力分开，而不是做成一个大而全的管理 API：

| 端点 | 作用 |
| --- | --- |
| `/ops/health` | 运行时健康状态 |
| `/ops/status` | 当前服务与模块状态 |
| `/ops/metrics` | 指标导出 |
| `/ops/profile` | 采样或短时 profile |
| `/ops/config` | 只读配置快照 |

## 节点状态与心跳

本地 service 不做 per-service heartbeat。其存活与清理由 runtime 的 service stop/exit、registry 注销和 handle 失效流程维护。

IPC / cluster 节点状态由链路 heartbeat 驱动：

```text
online -> suspect -> offline -> removed
```

默认建议：

```text
heartbeat_interval = 2s
suspect_after      = 3 missed heartbeats
offline_after      = 5 missed heartbeats
remove_after       = 60s after offline
```

`shield_ops` 应暴露：

- 当前 node 列表和状态。
- 最近一次 heartbeat 时间。
- heartbeat RTT 和 miss 计数。
- offline 节点 tombstone。
- 因 node offline 失败的 pending call 数量。

## 数据流

```text
shield_core / modules
  -> internal collectors
  -> shield_ops
  -> console / HTTP / exporter / profile
```

`shield_core` 不应该直接依赖 `shield_ops`。正确方向是：`shield_ops` 读取、聚合和导出运行时状态，但不反向污染核心语义。

## 与监控文档的关系

`docs/monitoring.md` 只描述可观测性结果和指标概念。`docs/ops.md` 描述的是运维、诊断和 profile 的完整设计层。

换句话说：

- `monitoring` 关注“看见什么”。
- `ops` 关注“怎么查、怎么采样、怎么控制”。
