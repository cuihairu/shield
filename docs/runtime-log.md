# 日志运行时语义

本文档包含 Shield 日志系统相关的运行时语义决策。

## 设计原则

- 日志是结构化数据，不是纯文本。（设计目标；当前实现为单行文本格式，见下文「日志格式」。）
- 日志包含上下文信息（service_id 前缀、trace_id）。
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

-- 传 table：整体序列化为 JSON 形态写入（单参数）
shield.log.info({
    event = "player_login",
    uid = uid,
    ip = ip,
})
```

注意：`shield.log.*` 绑定只接受**单个参数**（`src/lua/lua_api.cpp` 的 `register_log_api`）。`shield.log.info("msg", { ... })` 这种两参调用未实现——第二个参数会被静默丢弃。带上下文时请把上下文并入唯一的 table 参数（消息文本可用 `event`/`msg` 字段承载）。

## 日志格式

### 结构化日志格式（目标态，未实现）

以下 JSON 形态是设计目标；当前实现没有 JSON 结构化输出，唯一实现是下文的单行文本格式（`src/log/logger.cpp` 的 `format_record`）。

```json
{
  "timestamp": "2026-06-10T12:00:00.123Z",
  "level": "info",
  "message": "player login",
  "service": "gateway",
  "service_id": 1,
  "trace_id": "req-12345",
  "node_id": "node-1",
  "context": {
    "uid": "user123",
    "ip": "192.168.1.100"
  }
}
```

### 文本格式（当前唯一实现）

`format_record` 输出的单行文本：

```txt
<epoch毫秒> [LEVEL] <logger名>: <消息正文> [(文件:行号)][ trace=<trace_id>]
```

示例：

```txt
1760000000123 [INFO] lua: [gateway] {"event":"player_login","uid":"user123"}
1760000000456 [ERROR] bootstrap: Failed to load config: app.yaml (/home/…/src/bootstrap/bootstrap.cpp:424) trace=t-1a2b3c
```

字段说明：

- `timestamp`: epoch 毫秒整数（非 ISO 8601）
- `level`: 日志级别（大写：DEBUG/INFO/WARN/ERROR/FATAL）
- `logger名`: logger 实例名；Lua 侧 `shield.log.*` 固定走 `lua` logger
- 消息正文：Lua 侧为参数的 JSON 形态序列化，服务内执行时自动加 `[service_id] ` 前缀（见下文「上下文注入」）
- `(文件:行号)` / `trace=<id>`：仅在有值时附带；文件是宏传入的 `__FILE__`（绝对路径，如 `(/home/…/src/bootstrap/bootstrap.cpp:424)`）

## 上下文注入

当前实现的自动注入只有两项：

| 字段 | 来源 | 形态 |
|------|------|------|
| `trace_id` | 消息追踪（call/send 路径生成与传播） | 行尾 ` trace=<id>` |
| `service_id` | Lua 侧当前 service | 消息正文前的 `[service_id] ` 前缀（Lua binding 注入；C++ 直写日志无此前缀） |

`service` 名称、`node_id`、`request_id` 作为独立自动注入字段未实现。

## 日志配置

完整配置 schema 见 [配置语义](runtime-config.md#phase-1-schema) 中 `log` 部分。

### 输出目标

`log.targets` 多输出目标**未实现**（无任何代码读取该键）。实际 sink 固定为两类，由 bootstrap 加载配置后重建（`src/bootstrap/bootstrap.cpp` 的 `apply_sinks`）：

- console sink：`log.console`（默认 `true`）
- rotating file sink（可选）：`log.file.enabled`（默认 `false`）、`log.file.path`（默认 `logs/shield.log`）

全局只有一份级别开关（`log.level`），不支持按目标独立设级别或按 service 过滤：

```yaml
log:
  level: info
  console: true
  file:
    enabled: true
    path: "logs/shield.log"
    max_size_mb: 100
    max_files: 10
```

## 日志轮转

### 按大小轮转

唯一实现的轮转方式（`src/log/logger.cpp` 的 `RotatingFileSink`）：超过 `max_size_mb` 即轮转。

```yaml
log:
  file:
    enabled: true
    path: "logs/shield.log"
    max_size_mb: 100             # 单文件上限（MB），默认 100
    max_files: 10                # 保留副本数，默认 10
```

文件命名：

```txt
logs/shield.log          # 当前文件
logs/shield.log.1        # 最近轮转
logs/shield.log.2
...
logs/shield.log.N        # 最旧（N = max_files）
```

### 按日期轮转（未实现）

`rotation: daily` / `compress` / `.gz` 均未实现——不存在按日期轮转，也没有压缩。只支持上文的按大小轮转与固定数量副本。

## 性能考虑

- 日志写入是**同步**的：每条日志在调用线程内写完所有 sink（全局互斥锁串行，`src/log/logger.cpp` 的 `Logger::log`）。没有异步队列、没有缓冲区，也不存在 `log.buffer_size` / `log.flush_interval` 配置键。
- 高频日志路径的开销主要是字符串拼接与 I/O；文件 sink 每条 flush。

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
shield.log.error({
    msg = "db query failed",
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
    shield.log.error({
        msg = "operation failed",
        error = tostring(err),
        stack = debug.traceback(),  -- Lua 堆栈
    })
end
```

## 审计日志

`log.audit` 配置段**未实现**（零消费：全仓无任何 `log.audit` 读取者，也没有独立的审计日志通道）。当前只能通过普通日志（可选文件 sink）自行约定审计事件的记录方式。以下为契约草案：

```yaml
log:
  audit:                         # 未实现
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

`GET /ops/logs/stats` **未实现**——ops HTTP 路由表（`src/console/ops_http_handler.cpp` 的 `register_routes`）没有注册该路由，请求返回 404。日志侧也不维护按级别计数或 recent errors 环形缓冲。以下为契约草案：

```json
GET /ops/logs/stats   // 未实现

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

**目标态，当前不可达**：以下示例都基于 `log.targets`，而 `log.targets` 未实现（无代码读取）。当前只有 console 与可选文件 sink，外部集成属于可选扩展——外部系统只能直接读文件/控制台再自行采集。

### ELK Stack（目标态，未实现）

```yaml
log:
  targets:                       # 未实现
    - type: elasticsearch
      hosts: ["http://localhost:9200"]
      index: "shield-logs"
      level: info
```

### Loki（目标态，未实现）

```yaml
log:
  targets:                       # 未实现
    - type: loki
      url: "http://localhost:3100/loki/api/v1/push"
      labels:
        app: shield
        env: production
```
