# 错误码参考

本文档汇总 Shield 运行时所有错误码，是错误码的权威来源。其他文档引用本文档，不重复列出。

## 错误对象结构

所有 Runtime 错误通过 `ok == false` 返回，第二个返回值是 Error 对象：

```lua
{
  code = "timeout",           -- 错误码（本文档定义）
  message = "call timeout",   -- 人类可读描述
  source = "runtime",         -- 来源：runtime | data | network
  retryable = false,          -- 是否建议重试
}
```

## 一、消息与服务错误

`shield.send` / `shield.call` / `shield.spawn` 产生的错误。

| 错误码 | 来源 | 说明 | retryable |
|--------|------|------|-----------|
| `invalid_target` | send/call | 目标格式错误（非 handle 且非合法 name） | 否 |
| `invalid_method` | send/call | 方法名非法（空、过长、含非法字符） | 否 |
| `invalid_module` | spawn | Lua 脚本路径无效或加载失败 | 否 |
| `invalid_name` | spawn | 服务名不合法（格式、长度、保留前缀） | 否 |
| `name_conflict` | spawn | 服务名已被占用 | 否 |
| `encode_failed` | send/call | 消息编码失败（类型不支持、嵌套过深、循环引用） | 否 |
| `message_too_large` | send/call | 消息体积超过 `max_message_size`（默认 1MB） | 否 |
| `service_not_found` | send/call | 目标服务不存在（name 未注册或 handle 已失效） | 是 |
| `service_dead` | send/call | 目标服务已停止 | 否 |
| `node_offline` | send/call | 目标节点离线（集群场景） | 是 |
| `mailbox_full` | send | 目标服务 mailbox 达到上限 | 是 |
| `init_failed` | spawn | `on_init` 返回失败或抛出异常 | 否 |
| `spawn_timeout` | spawn | 服务初始化超过 `spawn_timeout`（默认 10s） | 否 |
| `runtime_stopping` | send/call/spawn | 运行时正在关闭 | 否 |
| `permission_denied` | send/call/spawn | 权限不足 | 否 |
| `timeout` | call | 调用超时（默认 5s） | 是 |
| `method_not_found` | call | 目标服务没有该方法 | 否 |

## 二、资源限制错误

服务级资源超限产生的错误。

| 错误码 | 说明 | 默认上限 |
|--------|------|----------|
| `mailbox_full` | 单个 service mailbox 消息数超限 | 10000 |
| `coroutine_limit` | 单个 service coroutine 数超限 | 1000 |
| `pending_call_limit` | 单个 service 待响应 call 数超限 | 1000 |
| `timer_limit` | 单个 service timer 数超限 | 10000 |
| `fork_limit` | 单个 service fork task 数超限 | 1000 |

## 三、数据库错误

`shield.db.*` 产生的错误。

| 错误码 | 说明 | retryable |
|--------|------|-----------|
| `connection_lost` | 数据库连接丢失 | 是 |
| `connection_timeout` | 建立连接超时 | 是 |
| `query_timeout` | 查询超时 | 是 |
| `syntax_error` | SQL 语法错误 | 否 |
| `constraint_violation` | 约束违反（唯一键、外键等） | 否 |
| `transaction_aborted` | 事务中止 | 是 |
| `pool_exhausted` | 连接池耗尽 | 是 |

## 四、Redis 错误

`shield.redis.*` / `shield.global()` 等 Redis 相关 API 产生的错误。

| 错误码 | 说明 | retryable |
|--------|------|-----------|
| `connection_lost` | Redis 连接丢失 | 是 |
| `connection_timeout` | 建立连接超时 | 是 |
| `command_timeout` | 命令执行超时 | 是 |
| `wrong_type` | Redis 类型错误（如对 String 执行 List 操作） | 否 |
| `pool_exhausted` | 连接池耗尽 | 是 |

## 五、网络错误

`shield_net` / `SessionHandle` 相关的错误。

| 错误码 | 说明 | retryable |
|--------|------|-----------|
| `session_closed` | session 已关闭，handle stale | 否 |
| `session_send_queue_full` | session 发送队列已满 | 是 |
| `handshake_timeout` | 握手超时 | 否 |
| `decode_error` | 协议解码错误 | 否 |
| `connection_limit` | 连接数达到上限 | 是 |
| `ip_limit` | 单 IP 连接数达到上限 | 否 |

## 六、错误处理建议

### 重试策略

```lua
function M.call_with_retry(target, method, data, max_retries)
    max_retries = max_retries or 3
    local retries = 0

    while retries < max_retries do
        local ok, result = shield.call(target, method, data)
        if ok then
            return result
        end

        if result.retryable then
            retries = retries + 1
            if retries < max_retries then
                shield.sleep(100 * retries)  -- 指数退避
            end
        else
            return nil, result
        end
    end

    return nil, { code = "max_retries_exceeded", message = "Max retries exceeded" }
end
```

### 错误日志

```lua
-- Runtime 错误
shield.log.error(string.format(
    "[runtime] %s: target=%s method=%s code=%s message=%s",
    "call_failed", target, method, err.code, err.message
))

-- 业务错误
shield.log.warn(string.format(
    "[business] %s: target=%s method=%s code=%s message=%s",
    "business_error", target, method, err.code, err.message
))
```

### 业务错误

业务错误由 callee 业务代码定义，通过 `call` 返回值传递，不使用本文档定义的错误码：

```lua
-- Callee
function M.get_player(uid)
    local player = find_player(uid)
    if not player then
        return nil, { code = "PLAYER_NOT_FOUND", message = "Player not found" }
    end
    return player
end

-- Caller
local ok, result, err = shield.call("player", "get_player", uid)
if ok then
    if result == nil then
        -- 业务错误
        shield.log.warn("business error: " .. err.code)
    else
        process_player(result)
    end
else
    -- Runtime 错误（本文档定义的错误码）
    shield.log.error("runtime error: " .. result.code)
end
```
