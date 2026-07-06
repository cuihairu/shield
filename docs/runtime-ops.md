# 运维运行时语义

本文档包含 Shield 运维和调试相关的运行时语义决策。

当前状态：

- `shield_ops` 是 official optional module 契约。
- 它不提供业务 Lua API，也不属于最小运行路径。
- 横向 owner、配置归属和 disabled 语义见 [官方可选模块契约](optional-modules.md)。

## 设计目标

`shield_ops` 不给游戏业务提供新 API，而是让运行时在不侵入核心语义的前提下，可被观察、诊断、采样和控制。

- 观察运行时内部状态，而不暴露 CAF 细节给业务脚本。
- 在开发、测试、预发环境提供足够的诊断信息。
- 在生产环境提供受控、低开销、可鉴权的运维入口。
- 所有 ops 能力都应可关闭，不影响 `shield_core` 启动。

典型能力：

| 能力 | 说明 |
| --- | --- |
| Metrics | 计数器、直方图、gauge，用于吞吐、延迟、错误率 |
| Diagnostics | 服务列表、actor 数量、pending call、timer、Lua VM 池、连接数 |
| Console | 交互式调试控制台，查询、调用、列服务、看状态 |
| Profile | 采样式热点分析、消息延迟剖面、慢调用追踪 |
| Health | 进程存活、关键模块就绪、资源状态 |

Lua 诊断控制台与 Lua 内存观测是 `Console` / `Diagnostics` 的专项能力，单独设计见 [Lua 诊断控制台设计](ops-lua-console.md)。

## 可观测性现状与非目标

当前产品只保留日志模块（控制台日志；文件日志作为 `shield_log` 的可选 sink，默认关闭；Lua 目标 API `shield.log.info/warn/error/debug`）。日志不是 `shield_core` 语义的一部分。

Core 当前**不提供**：Prometheus metrics、HealthCheckRegistry、`/health` `/status` `/metrics` HTTP 端点、运行时诊断 HTTP API、配置热重载监控器。这些归入 `shield_ops` 官方可选模块或独立扩展，且不能反向污染 runtime core。

## shield_ops 默认策略

`shield_ops` 不属于 core，但作为官方可选模块保留。

默认策略：

- 开发环境可本地启用。
- 生产环境默认关闭远程控制入口。
- metrics 可以独立启用。
- profile 必须显式启用。
- console 必须显式启用。
- HTTP 只用于管理入口，不作为业务 HTTP server。

core 只提供可读取的 runtime snapshot 和 counters，不反向依赖 `shield_ops`。

## Public Surface

`shield_ops` 的 public surface 只有管理入口：

```text
HTTP debug endpoints
local admin socket
console
metrics exporter
profile controls
```

它不定义 `shield.ops.*` 业务 Lua API。
它也不提供业务 REST router、middleware chain 或 Web framework 集成。

## 运维端点

`shield_ops` 在显式启用 HTTP 管理入口时暴露以下端点：

| 端点 | 方法 | 说明 |
|------|------|------|
| `/ops/health` | GET | 健康检查 |
| `/ops/status` | GET | 运行时状态 |
| `/ops/metrics` | GET | 指标导出（Prometheus 格式） |
| `/ops/services` | GET | 服务列表 |
| `/ops/services/:name` | GET | 服务详情 |
| `/ops/profile` | POST | 启动/停止 profile |
| `/ops/config` | GET | 当前配置快照 |

如果启用了 Lua 诊断控制台，管理面还可以额外暴露只读 Lua 观测能力，例如：

| 能力 | 形态 | 说明 |
|------|------|------|
| `lua.inspect <service> summary` | console | 查看 service 对应 Lua VM 的摘要 |
| `lua.snapshot <service>` | console / HTTP | 记录 Lua 内存摘要快照 |
| `lua.diff <service> <a> <b>` | console / HTTP | 比较两次 Lua 快照 |

任意 Lua 执行不是默认能力；如后续支持 `lua.eval`，也必须显式启用并默认仅限本地入口。详见 [Lua 诊断控制台设计](ops-lua-console.md)。

### 健康检查

```
GET /ops/health
```

响应：

```json
{
  "status": "ok",  // ok | degraded | unhealthy
  "uptime": 3600,
  "checks": {
    "core": { "status": "ok" },
    "database": { "status": "ok", "latency_ms": 5 },
    "redis": { "status": "ok", "latency_ms": 2 },
    "cluster": { "status": "ok", "nodes": 3 }
  }
}
```

### 运行时状态

```
GET /ops/status
```

响应：

```json
{
  "app": { "name": "my_game", "version": "1.0.0" },
  "runtime": {
    "uptime": 3600,
    "pid": 12345,
    "node_id": "node-1"
  },
  "services": {
    "total": 10,
    "by_type": {
      "gateway": 1,
      "player": 5,
      "room": 4
    }
  },
  "resources": {
    "lua_vms": 10,
    "connections": 1500,
    "pending_calls": 5,
    "timers": 20
  }
}
```

### 指标导出

```
GET /ops/metrics
```

响应（Prometheus 格式）：

```
# HELP shield_requests_total Total requests
# TYPE shield_requests_total counter
shield_requests_total{service="player",method="get_info"} 1234

# HELP shield_request_duration_seconds Request duration
# TYPE shield_request_duration_seconds histogram
shield_request_duration_seconds_bucket{service="player",method="get_info",le="0.01"} 1000
shield_request_duration_seconds_bucket{service="player",method="get_info",le="0.1"} 1200

# HELP shield_connections Current connections
# TYPE shield_connections gauge
shield_connections 1500

# HELP shield_mailbox_size Mailbox size
# TYPE shield_mailbox_size gauge
shield_mailbox_size{service="player"} 10
```

### 服务列表

```
GET /ops/services
```

响应：

```json
{
  "services": [
    {
      "name": "gateway",
      "id": 1,
      "type": "gateway",
      "status": "running",
      "uptime": 3600,
      "requests": 10000,
      "errors": 5
    },
    {
      "name": "player.1",
      "id": 2,
      "type": "player",
      "status": "running",
      "uptime": 3600,
      "requests": 5000,
      "errors": 0
    }
  ]
}
```

### 服务详情

```
GET /ops/services/:name
```

响应：

```json
{
  "name": "player.1",
  "id": 2,
  "type": "player",
  "status": "running",
  "uptime": 3600,
  "stats": {
    "requests": 5000,
    "errors": 0,
    "pending_calls": 2,
    "timers": 5,
    "coroutines": 3,
    "mailbox_size": 0
  },
  "last_error": null
}
```

## ops 安全

ops 暴露必须遵守：

- 默认绑定 localhost 或 local admin socket。
- 远程访问必须鉴权。
- 不默认输出完整 payload。
- 对敏感字段做 redaction。
- 控制类 API 需要单独权限。
- profile 和 dump 需要速率限制。

允许暴露：

```txt
service list
registry names
mailbox size
pending calls
timer count
coroutine count
node heartbeat status
slow call summary
```

禁止默认暴露：

```txt
CAF actor handle
完整业务 payload
密钥、token、密码
未鉴权远程 console
```

## 配置示例

```yaml
ops:
  enabled: true
  bind: "127.0.0.1:9090"  # 仅本地访问
  metrics: true
  health: true
  profile: false  # 生产环境默认关闭
  console: false  # 生产环境默认关闭
  auth:           # 远程访问鉴权（可选）
    type: token
    token: ${OPS_TOKEN}
```

## 节点状态与心跳

本地 service 不做 per-service heartbeat；其存活与清理由 runtime 的 service stop/exit、registry 注销和 handle 失效流程维护。

IPC / cluster 节点状态由链路 heartbeat 驱动：`online -> suspect -> offline -> removed`。默认建议：

```text
heartbeat_interval = 2s
suspect_after      = 3 missed heartbeats
offline_after      = 5 missed heartbeats
remove_after       = 60s after offline
```

`shield_ops` 应暴露当前 node 列表与状态、最近一次 heartbeat 时间、heartbeat RTT 与 miss 计数、offline 节点 tombstone、因 node offline 失败的 pending call 数量。

## 数据流

```text
shield_core / modules
  -> internal collectors
  -> shield_ops
  -> console / HTTP / exporter / profile
```

`shield_core` 不直接依赖 `shield_ops`；`shield_ops` 只读取、聚合和导出运行时状态。
