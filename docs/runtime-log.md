# 日志运行时语义

本文档包含 Shield 日志系统相关的运行时语义决策。

## 设计原则

- 日志是结构化数据，不是纯文本。
- 日志包含上下文信息（service、request_id）。
- 日志级别语义明确。
- 日志不影响业务逻辑（不抛错、不阻塞）。
- 敏感数据不写入日志。

## 日志级别

| 级别 | 用途 | 生产环境默认 |
|------|------|--------------|
| `debug` | 开发调试信息 | 关闭 |
| `info` | 正常运行信息 | 开启 |
| `warn` | 警告（可恢复） | 开启 |
| `error` | 错误（需关注） | 开启 |

级别语义：

- `debug`: 变量值、函数调用轨迹、请求详情。开发时启用，生产关闭。
- `info`: 服务启动/停止、连接建立/断开、重要业务事件。
- `warn`: 可恢复的异常、降级运行、重试成功。
- `error`: 不可恢复的错误、需要人工介入。

## Lua API

```lua
-- 基础日志
shield.log.debug("message")
shield.log.info("message")
shield.log.warn("message")
shield.log.error("message")

-- 格式化日志
shield.log.info(string.format("player %s login from %s", uid, ip))
shield.log.error(string.format("db query failed: %s", err))

-- 带上下文的日志
shield.log.info("player login", {
    uid = uid,
    ip = ip,
    time = shield.now(),
})
```

## 日志格式

### 结构化日志格式

```json
{
  "timestamp": "2026-06-10T12:00:00.123Z",
  "level": "info",
  "message": "player login",
  "service": "gateway",
  "service_id": 1,
  "request_id": "req-12345",
  "node_id": "node-1",
  "context": {
    "uid": "user123",
    "ip": "192.168.1.100"
  }
}
```

### 文本格式（控制台输出）

```txt
2026-06-10 12:00:00.123 [INFO] [gateway:1] player login uid=user123 ip=192.168.1.100
```

字段说明：

- `timestamp`: ISO 8601 格式，UTC 时区
- `level`: 日志级别（大写）
- `service`: 服务名称
- `service_id`: 服务 ID
- `request_id`: 请求追踪 ID（如有）
- `context`: 附加上下文（key=value 格式）

## 上下文注入

日志系统自动注入以下上下文：

| 字段 | 来源 | 说明 |
|------|------|------|
| `service` | 当前 service | 服务名称 |
| `service_id` | 当前 service | 服务 ID |
| `node_id` | 配置 | 节点 ID |
| `request_id` | 消息追踪 | 请求 ID（call/send 时生成） |

Lua 中不需要手动传入这些字段。

## 日志配置

完整配置 schema 见 [配置语义](runtime-config.md#完整配置-schema) 中 `log` 部分。

### 输出目标

支持多输出目标，每个目标可独立配置级别和格式：

```yaml
log:
  targets:
    - type: stdout
      format: text
      level: info
    - type: file
      path: "logs/error.log"
      level: error               # 只记录 error
    - type: file
      path: "logs/debug.log"
      level: debug
      services: ["gateway"]      # 只记录 gateway 的 debug
```

## 日志轮转

### 按大小轮转

```yaml
log:
  file:
    rotation: size
    max_size: 100                # 100MB
    max_files: 10                # 保留 10 个文件
```

文件命名：

```txt
logs/shield.log          # 当前文件
logs/shield.log.1        # 最近轮转
logs/shield.log.2
...
logs/shield.log.10       # 最旧
```

### 按日期轮转

```yaml
log:
  file:
    rotation: daily
    max_files: 30                # 保留 30 天
    compress: true
```

文件命名：

```txt
logs/shield.log                  # 当前文件
logs/shield.log.2026-06-09.gz   # 昨天
logs/shield.log.2026-06-08.gz   # 前天
```

## 性能考虑

- 日志写入是异步的，不阻塞业务线程。
- 日志缓冲区默认 8KB，满时批量写入。
- 高频日志场景可调整缓冲区大小：

```yaml
log:
  buffer_size: 65536             # 64KB 缓冲区
  flush_interval: 1000           # 最长 1 秒刷新一次
```

## 敏感数据处理

### 自动脱敏

日志系统不自动脱敏，由业务层负责。

### 禁止记录

以下数据禁止写入日志：

- 密码、密钥、token
- 完整的信用卡号
- 身份证号（可记录后四位）
- 完整的请求/响应 payload（debug 级别除外）

### 推荐做法

```lua
-- 不推荐
shield.log.info("login: " .. username .. " password: " .. password)

-- 推荐
shield.log.info("login: " .. username)

-- 脱敏记录
shield.log.debug("password length: " .. #password)
```

## 错误日志

### 格式

```lua
shield.log.error("db query failed", {
    error = err.message,
    code = err.code,
    query = "SELECT * FROM users",  -- 不记录参数（可能含敏感数据）
    duration_ms = duration,
})
```

### 错误堆栈

```lua
local ok, err = pcall(function()
    -- 可能出错的代码
end)

if not ok then
    shield.log.error("operation failed", {
        error = err,
        stack = debug.traceback(),  -- Lua 堆栈
    })
end
```

## 审计日志

重要业务操作单独记录审计日志：

```yaml
log:
  audit:
    enabled: true
    path: "logs/audit.log"
    events:
      - "login"
      - "logout"
      - "purchase"
      - "password_change"
      - "admin_action"
```

审计日志格式：

```json
{
  "timestamp": "2026-06-10T12:00:00.123Z",
  "event": "login",
  "uid": "user123",
  "ip": "192.168.1.100",
  "result": "success",
  "details": {
    "method": "password"
  }
}
```

## ops 集成

日志统计暴露给 ops：

```json
GET /ops/logs/stats

{
  "total": 123456,
  "by_level": {
    "debug": 0,
    "info": 120000,
    "warn": 3000,
    "error": 456
  },
  "recent_errors": [
    {
      "timestamp": "2026-06-10T12:00:00Z",
      "service": "gateway",
      "message": "connection timeout"
    }
  ]
}
```

## 与外部日志系统集成

### ELK Stack

```yaml
log:
  targets:
    - type: elasticsearch
      hosts: ["http://localhost:9200"]
      index: "shield-logs"
      level: info
```

### Loki

```yaml
log:
  targets:
    - type: loki
      url: "http://localhost:3100/loki/api/v1/push"
      labels:
        app: shield
        env: production
```

当前最小契约只要求文件和控制台输出；外部集成属于可选扩展。
