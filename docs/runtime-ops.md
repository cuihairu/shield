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

Core 不内建：Prometheus metrics、HealthCheckRegistry、`/metrics` 端点、配置热重载监控器。这些归入 `shield_ops` 官方可选模块或独立扩展，且不能反向污染 runtime core。

已落地的运维面（实现于 `shield_bootstrap` 的 console 组件，`http.enabled: true` 时生效）：

- Unix socket 诊断控制台（`root.*` / Lua 命令，见 [Lua 诊断控制台设计](ops-lua-console.md)）
- HTTP ops 端点：`/ops/health`、`/ops/status`、`/ops/metrics`、`/ops/services`、`/ops/plugins`、`/ops/config`、`/ops/eval`
- Lua 业务侧 `shield.httpd.*` 可注册自有管理端点（见 [Lua API 契约](lua-api.md)）

HTTP ops 服务端安全基线：

- `http.host` 默认 `127.0.0.1`（环回）；对外暴露必须显式配置并在前置层（防火墙/反代）再加控制。
- `/ops/eval` 为远程代码入口，**默认不注册**（请求 404）。启用需同时满足：
  - `http.eval_enabled: true`
  - `http.eval_token: <非空 token>`，否则启动时报错且路由不注册
- 已启用的 `/ops/eval` 要求 `Authorization: Bearer <token>`（常量时间比较），未授权请求返回 401。
- eval 代码运行在受限 VM 中：`os.execute`/`os.exit`/`os.getenv`/`os.remove`/`os.rename`/`os.setlocale`、`io` 库、`require`/`package` 均被移除（`os.time`/`os.date`/`os.clock` 保留）。

注意：这些能力当前编译在 `shield_bootstrap` 而非 `shield_ops` 空壳 target 内；模块归属对齐是后续工作。轻量 `/ops/health` 探针（进程内读取、不经 Lua actor 往返，actor 网格卡死时仍可应答）、`/ops/metrics` Prometheus 导出与 `/ops/services/:name` 服务详情已提供（P0）；`/ops/profile` 尚未提供。

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
| `/ops/health` | GET | 健康检查（已提供） |
| `/ops/status` | GET | 运行时状态 |
| `/ops/metrics` | GET | 指标导出（Prometheus 格式，已提供） |
| `/ops/services` | GET | 服务列表 |
| `/ops/services/:name` | GET | 服务详情（已提供） |
| `/ops/profile` | POST | 启动/停止 profile（尚未提供） |
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

P0 实现为轻量探针：全部读取在 HTTP 线程内进程内完成，不经 Lua actor 往返，因此 actor 网格卡死时探针仍可应答。P0 取舍：HTTP 恒 200，verdict 携带在 body 中（503 映射留后续）；`unhealthy` 档位保留给后续（如 actor 网格失联检测）。

响应（与其他 ops 端点一致的 `type`/`data` 信封）：

```json
{
  "type": "result",
  "data": {
    "status": "ok",  // ok | degraded；任一 check 非 ok 即 degraded
    "uptime": 3600.5,
    "checks": {
      "core": { "status": "ok", "uptime_seconds": 3600.5 },
      "plugins": { "status": "ok", "started": 2, "required_down": 0 }
    }
  }
}
```

checks 按编译开关与运行时状态裁剪：

- `core`：恒存在；uptime 秒数。
- `plugins`：恒存在；required 实例未达 `started` 即 degraded（optional 实例缺失不影响）。
- `server`（`SERVER=ON` 且 ServerManager 已初始化）：仅 `running` 计为 ok，`starting`/`maintenance`/`shutdown` 均 degraded。
- `cluster`（`CLUSTER=ON` 且 ClusterManager 已初始化）：有 Offline/Removed 节点即 degraded。
- `global`（`GLOBAL=ON` 且 GlobalManager 已初始化）：manager 存在即 ok。

与 server/cluster 一致的裁剪约定：模块已编译但本节点未启用该角色（manager 不存在）时省略该 check，而非 degrade——global 节点角色是部署形态，不是健康缺陷。`database`/`redis` 等依赖探活留后续。

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

P0 导出 Prometheus 0.0.4 文本格式（`Content-Type: text/plain; version=0.0.4; charset=utf-8`，HELP/TYPE 头齐全，标签值转义 `\` `"` `\n`）。进程内读取为主，唯一经 actor 网格的 `shield_services` 在 500ms 超时时省略——网格卡死不会拖死抓取。

按编译开关与运行时状态裁剪的指标族：

| 指标 | 类型 | 说明 |
|------|------|------|
| `shield_uptime_seconds` | gauge | 进程 uptime |
| `shield_plugin_instances{state}` | gauge | 插件实例按生命周期状态（planned/loaded/started/unavailable/failed/stopped） |
| `shield_services` | gauge | 已注册 Lua 服务数（actor 往返，500ms 超时省略） |
| `shield_service_requests_total{service}` | counter | 服务收到的消息数（send/call/system 合流，按服务本轮生命周期累计，respawn 归零） |
| `shield_service_errors_total{service}` | counter | 服务 handler 失败数（口径同上） |
| `shield_server_state{state}` | gauge | Server 状态机（SERVER=ON 且 manager 存在） |
| `shield_global_data_keys` / `shield_global_cache_entries` | gauge | global 数据键 / 本地缓存条目（GLOBAL=ON 且 manager 存在） |
| `shield_global_cache_hits_total` / `shield_global_cache_misses_total` | counter | 缓存命中/未命中 |
| `shield_global_queues{type}` | gauge | 队列数（normal/delay/priority/broadcast/reliable） |
| `shield_global_locks{kind}` | gauge | 存活锁（mutex/spinlock/rwlock） |
| `shield_global_rank_boards` / `shield_global_rank_members` | gauge | 排行榜板数 / 成员总数 |
| `shield_global_scheduler_tasks` / `shield_global_scheduler_active` | gauge | 调度任务总数 / 活跃数 |
| `shield_cluster_nodes{state}` | gauge | 集群节点按状态（CLUSTER=ON 且 manager 存在） |
| `shield_cluster_transport_connections` / `..._reconnects_total` | gauge/counter | 传输连接数 / 重连次数（transport 存在） |
| `shield_cluster_transport_messages_total{direction}` | counter | 收发消息数（tx/rx） |
| `shield_cluster_transport_heartbeats_total{direction}` | counter | 收发心跳数（tx/rx） |

`shield_requests_total`、`shield_request_duration_seconds` 等 per-service 流量指标不在 P0 范围，留后续。

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

P0 实测口径：注册表内只读快照，走 `""-id` forked task（与 `/ops/services` 列表同路），2s 超时返回 504。未发布的名字（on_init 中、已退出）与列表口径一致按不存在处理，返回 404。

响应：

```json
{
  "type": "result",
  "data": {
    "name": "gateway",
    "state": "running",
    "script": "/path/to/gateway.lua",
    "rpc_routes": 2,
    "requests": 1234,
    "errors": 2
  }
}
```

`requests`/`errors` 为该服务本轮生命周期的累计流量（spawn 归零、exit 移除）；`script` 仅配置态 runtime actor 有记录，spawn 服务可缺省。per-service `uptime`/`timers`/`coroutines` 等统计留后续。

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
