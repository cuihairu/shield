# 运维运行时语义

本文档包含 Shield 运维和调试相关的运行时语义决策。

当前状态：

- `shield_ops` 是 official optional module 设计稿。
- 它不提供业务 Lua API，也不属于最小运行路径。
- 横向 owner、配置归属和 disabled 语义见 [官方可选模块契约](optional-modules.md)。

## shield_ops 默认策略

`shield_ops` 不属于 core，但作为官方可选模块保留。

默认策略：

- 开发环境可本地启用。
- 生产环境默认关闭远程控制入口。
- metrics 可以独立启用。
- profile 必须显式启用。
- console 必须显式启用。

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

## 运维端点

`shield_ops` 通过 HTTP 端点暴露运维能力：

| 端点 | 方法 | 说明 |
|------|------|------|
| `/ops/health` | GET | 健康检查 |
| `/ops/status` | GET | 运行时状态 |
| `/ops/metrics` | GET | 指标导出（Prometheus 格式） |
| `/ops/services` | GET | 服务列表 |
| `/ops/services/:name` | GET | 服务详情 |
| `/ops/profile` | POST | 启动/停止 profile |
| `/ops/config` | GET | 当前配置快照 |

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
