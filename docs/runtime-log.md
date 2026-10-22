# Log Runtime Semantics

This document contains the runtime semantics decisions for Shield's logging system.

## Design Principles

- Logs are structured data, not plain text. (Design goal; the current implementation is a single-line text format, see "Log Format" below.)
- Logs carry context (the service_id prefix, trace_id).
- Log level semantics are explicit.
- Logging never affects business logic (no throwing, no blocking).
- Sensitive data never goes into logs.

## Log Levels

| Level | Purpose | Default in production |
|------|------|--------------|
| `debug` | development debugging info | off |
| `info` | normal operation info | on |
| `warn` | warnings (recoverable) | on |
| `error` | errors (need attention) | on |

Level semantics:

- `debug`: variable values, call traces, request details. Enabled in development, off in production.
- `info`: service start/stop, connection establishment/teardown, important business events.
- `warn`: recoverable anomalies, degraded operation, successful retries.
- `error`: unrecoverable errors, cases needing human intervention.

## Lua API

```lua
-- basic logging
shield.log.debug("message")
shield.log.info("message")
shield.log.warn("message")
shield.log.error("message")

-- formatted logging
shield.log.info(string.format("player %s login from %s", uid, ip))
shield.log.error(string.format("db query failed: %s", err))

-- passing a table: serialized wholesale into a JSON-like form (single argument)
shield.log.info({
    event = "player_login",
    uid = uid,
    ip = ip,
})
```

Note: the `shield.log.*` bindings accept only a **single argument** (`register_log_api` in `src/lua/lua_api.cpp`). Two-argument calls like `shield.log.info("msg", { ... })` are not implemented — the second argument is silently dropped. To attach context, merge it into the single table argument (the message text can ride in an `event`/`msg` field).

## Log Format

### Structured log format (target state, not implemented)

The following JSON shape is the design target; the current implementation has no JSON structured output — the only implementation is the single-line text format below (`format_record` in `src/log/logger.cpp`).

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

### Text format (the only current implementation)

The single-line text output of `format_record`:

```txt
<epoch milliseconds> [LEVEL] <logger name>: <message body> [(file:line)][ trace=<trace_id>]
```

Examples:

```txt
1760000000123 [INFO] lua: [gateway] {"event":"player_login","uid":"user123"}
1760000000456 [ERROR] bootstrap: Failed to load config: app.yaml (/home/…/src/bootstrap/bootstrap.cpp:424) trace=t-1a2b3c
```

Field notes:

- `timestamp`: an epoch-milliseconds integer (not ISO 8601)
- `level`: the log level (uppercase: DEBUG/INFO/WARN/ERROR/FATAL)
- `logger name`: the logger instance name; on the Lua side `shield.log.*` always uses the `lua` logger
- message body: on the Lua side, a JSON-form serialization of the argument; when executed inside a service an automatic `[service_id] ` prefix is added (see "Context Injection" below)
- `(file:line)` / `trace=<id>`: attached only when present; the file is the `__FILE__` passed by the macro (an absolute path, e.g. `(/home/…/src/bootstrap/bootstrap.cpp:424)`)

## Context Injection

The current implementation auto-injects only two items:

| Field | Source | Shape |
|------|------|------|
| `trace_id` | message tracing (generated and propagated along call/send paths) | ` trace=<id>` at line end |
| `service_id` | the current service on the Lua side | the `[service_id] ` prefix before the message body (injected by the Lua binding; C++ direct logging has no such prefix) |

`service` name, `node_id`, and `request_id` as separate auto-injected fields are not implemented.

## Log Configuration

For the full config schema see the `log` section of Configuration Semantics.

### Output Targets

The multi-output `log.targets` is **not implemented** (no code reads that key). The actual sinks are fixed to two kinds, rebuilt by bootstrap after loading the configuration (`apply_sinks` in `src/bootstrap/bootstrap.cpp`):

- console sink: `log.console` (default `true`)
- rotating file sink (optional): `log.file.enabled` (default `false`), `log.file.path` (default `logs/shield.log`)

There is a single global level switch (`log.level`); per-target levels or per-service filtering are not supported:

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

## Log Rotation

### Size-based rotation

The only implemented rotation method (`RotatingFileSink` in `src/log/logger.cpp`): rotate when `max_size_mb` is exceeded.

```yaml
log:
  file:
    enabled: true
    path: "logs/shield.log"
    max_size_mb: 100             # per-file cap (MB), default 100
    max_files: 10                # retained copies, default 10
```

File naming:

```txt
logs/shield.log          # current file
logs/shield.log.1        # most recently rotated
logs/shield.log.2
...
logs/shield.log.N        # oldest (N = max_files)
```

### Date-based rotation (not implemented)

`rotation: daily` / `compress` / `.gz` are all unimplemented — no date-based rotation and no compression. Only the size-based rotation and fixed copy count above are supported.

## Performance Considerations

- Log writes are **synchronous**: every log entry writes all sinks on the calling thread (serialized by a global mutex, `Logger::log` in `src/log/logger.cpp`). There is no async queue and no buffer, nor any `log.buffer_size` / `log.flush_interval` config keys.
- The cost of high-frequency logging paths is mainly string concatenation and I/O; the file sink flushes per entry.

## Sensitive Data Handling

### Automatic redaction

The logging system does not redact automatically; that is the business layer's responsibility.

### Never log

The following must never be written to logs:

- passwords, keys, tokens
- full credit card numbers
- government ID numbers (the last four digits may be logged)
- full request/response payloads (except at debug level)

### Recommended practice

```lua
-- not recommended
shield.log.info("login: " .. username .. " password: " .. password)

-- recommended
shield.log.info("login: " .. username)

-- redacted logging
shield.log.debug("password length: " .. #password)
```

## Error Logging

### Format

```lua
shield.log.error({
    msg = "db query failed",
    error = err.message,
    code = err.code,
    query = "SELECT * FROM users",  -- parameters are not logged (may contain sensitive data)
    duration_ms = duration,
})
```

### Error stack traces

```lua
local ok, err = pcall(function()
    -- code that may fail
end)

if not ok then
    shield.log.error({
        msg = "operation failed",
        error = tostring(err),
        stack = debug.traceback(),  -- Lua stack
    })
end
```

## Audit Logs

The `log.audit` config section is **not implemented** (zero consumers: no reader of `log.audit` anywhere in the repo, and no separate audit log channel). Currently audit events can only be recorded by convention through ordinary logging (with the optional file sink). The following is a contract draft:

```yaml
log:
  audit:                         # not implemented
    enabled: true
    path: "logs/audit.log"
    events:
      - "login"
      - "logout"
      - "purchase"
      - "password_change"
      - "admin_action"
```

Audit log format:

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

## ops Integration

`GET /ops/logs/stats` is **not implemented** — the ops HTTP route table (`register_routes` in `src/console/ops_http_handler.cpp`) does not register that route, and requests return 404. The logging side also maintains no per-level counters or recent-errors ring buffer. The following is a contract draft:

```json
GET /ops/logs/stats   // not implemented

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

## Integration with External Log Systems

**Target state, unreachable today**: the examples below are all based on `log.targets`, which is not implemented (no code reads it). Currently there are only the console and optional file sinks; external integration is an optional extension — external systems can only read the file/console directly and collect on their own.

### ELK Stack (target state, not implemented)

```yaml
log:
  targets:                       # not implemented
    - type: elasticsearch
      hosts: ["http://localhost:9200"]
      index: "shield-logs"
      level: info
```

### Loki (target state, not implemented)

```yaml
log:
  targets:                       # not implemented
    - type: loki
      url: "http://localhost:3100/loki/api/v1/push"
      labels:
        app: shield
        env: production
```
