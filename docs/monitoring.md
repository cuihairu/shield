# 可观测性

Shield 提供多层次的可观测能力：运行时诊断、健康检查、Prometheus 指标、调试控制台、日志系统。

## 运行时诊断

`RuntimeDiagnostics` 通过 HTTP 端点暴露运行时状态，无需额外工具即可查看系统内部。

### HTTP 端点

| 端点 | 用途 |
|------|------|
| `GET /health` | 基础健康检查，返回 `{"status": "ok"}` |
| `GET /health/detailed` | 详细状态，包含已注册服务列表 |
| `GET /status` | 运行时状态：actor 数量、actor 详情、服务列表 |
| `GET /status/config` | 配置重载范围说明 |

### 示例响应

```bash
curl http://localhost:8082/health
# {"status":"ok"}

curl http://localhost:8082/status
# {"actor_count":12,"actors":[...],"services":["player_manager","room_manager"]}
```

## 健康检查

`HealthCheckRegistry`（shield_extensions）注册多个健康指标：

- **磁盘检查**: 可用磁盘空间
- **数据库检查**: 数据库连接可用性（如果配置了数据库）
- **应用检查**: 服务启动状态、actor 数量

通过 `--enable-health-check` 或配置启用。

## Prometheus 指标

通过 `--enable-metrics` 启用。集成 Prometheus C++ client，暴露标准 metrics 端点。

```yaml
metrics:
  enabled: true
  port: 9090
```

收集的指标类型：
- HTTP 请求延迟分布
- Actor 消息处理计数
- 网络连接数
- Lua VM 内存使用

## 调试控制台

TCP 13000 端口的交互式调试控制台，用于运行时检查。

```bash
telnet localhost 13000
```

### 命令

| 命令 | 用途 |
|------|------|
| `list` | 列出所有已注册服务 |
| `info <name>` | 查看服务详情 |
| `send <name> <json>` | 向服务发送异步消息 |
| `call <name> <json>` | 同步调用服务 |
| `nodes` | 列出集群节点 |

## 日志系统

基于 Boost.Log 的多目标日志：

- **控制台输出**: 开发阶段实时查看
- **文件输出**: 生产环境持久化
- **动态级别**: 运行时调整日志级别（通过配置重载）

### 配置

```yaml
log:
  global_level: "info"
  console:
    enabled: true
  file:
    enabled: true
    path: "logs/shield.log"
    rotation_size_mb: 100
```

### 日志级别

`trace` → `debug` → `info` → `warning` → `error` → `fatal`

## 配置重载

`FileWatcher` 监控配置文件变更，支持运行时重载部分配置项（日志级别等）。通过 `GET /status/config` 查看可重载范围。
