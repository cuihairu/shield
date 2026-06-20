# 错误码参考

本文档汇总 Shield runtime error 的稳定错误码，是错误码的权威来源。业务错误不由本文档统一分配。

实现快照：以下错误码中，标记 ✅ 的已在代码中使用并有测试覆盖；标记 ⚠️ 的在代码中使用但错误码字符串可能不完全匹配；标记 ❌ 的属于 Phase 2+，当前未实现。

## 错误对象结构

所有 runtime 错误通过 `ok == false` 返回，第二个返回值是 `Error` 对象：

```lua
{
  code = "timeout",           -- 错误码（本文档定义）
  message = "call timeout",   -- 人类可读描述
  -- 以下字段属于 Phase 2 扩展，当前未填充：
  -- source = "runtime",      -- 来源：runtime | data | network
  -- retryable = false,       -- 是否建议重试
  -- detail = nil,            -- 可选调试信息
}
```

规则：

- 本文档只定义 runtime/framework 错误。
- `shield.call` 成功返回时，后续返回值全部属于 callee 的业务契约，不由 runtime 解释。
- 旧 API 删除后的报错也使用本文档中的稳定错误码。

## 一、消息与服务错误

`shield.send` / `shield.call` / `shield.spawn` 产生的错误。

| 错误码 | 来源 | 说明 | retryable | 状态 |
|--------|------|------|-----------|------|
| `invalid_target` | send/call | 目标格式错误（非 handle 且非合法 name） | 否 | ✅ |
| `invalid_method` | send/call | 方法名非法（空、过长、含非法字符） | 否 | ❌ Phase 2 |
| `invalid_service_module` | spawn | Lua 文件未返回合法 service module table | 否 | ✅ |
| `script_load_failed` | spawn | Lua 文件语法错误、load 失败或顶层代码抛错 | 否 | ✅ |
| `invalid_name` | spawn | 服务名不合法（格式、长度、保留前缀） | 否 | ✅ |
| `name_conflict` | spawn | 服务名已被占用 | 否 | ✅ |
| `encode_failed` | send/call | 消息编码失败（类型不支持、嵌套过深、循环引用） | 否 | ❌ Phase 2 |
| `message_too_large` | send/call | 消息体积超过 `max_message_size`（默认 1MB） | 否 | ❌ Phase 2 |
| `service_not_found` | send/call | 目标服务不存在（name 未注册或 handle 已失效） | 是 | ✅ |
| `service_dead` | send/call | 目标服务已停止 | 否 | ❌ Phase 2 |
| `node_offline` | send/call | 目标节点离线（集群场景） | 是 | ❌ Cluster |
| `mailbox_full` | send | 目标服务 mailbox 达到上限 | 是 | ✅ |
| `init_failed` | spawn | `on_init` 返回失败或抛出异常 | 否 | ⚠️ 通用错误 |
| `spawn_timeout` | spawn | 服务初始化超过 `spawn_timeout`（默认 10s） | 否 | ❌ Phase 2 |
| `runtime_stopping` | send/call/spawn | 运行时正在关闭 | 否 | ❌ Phase 2 |
| `permission_denied` | send/call/spawn | 权限不足 | 否 | ❌ Phase 2 |
| `timeout` | call | 调用超时（默认 5s） | 是 | ✅ |
| `method_not_found` | call | 目标服务没有该方法 | 否 | ✅ |
| `handler_error` | call | 目标服务 method 抛出未捕获异常 | 否 | ⚠️ 通用错误 |
| `context_expired` | context | handler 已返回，`shield.sender/trace/deadline` 上下文失效 | 否 | ❌ Phase 2 |
| `api_not_allowed_in_exit` | exit hook | 在 `on_exit` 中调用了会挂起的 API | 否 | ✅ |
| `legacy_api_removed` | legacy API | 调用了已删除的旧 API | 否 | ✅ |

## 二、资源限制错误

服务级资源超限产生的错误。

| 错误码 | 说明 | 默认上限 | 状态 |
|--------|------|----------|------|
| `mailbox_full` | 单个 service mailbox 消息数超限 | 1000 | ✅ |
| `coroutine_limit` | 单个 service coroutine 数超限 | 1000 | ❌ Phase 2 |
| `pending_call_limit` | 单个 service 待响应 call 数超限 | 1000 | ❌ Phase 2 |
| `timer_limit` | 单个 service timer 数超限 | 10000 | ❌ Phase 2 |
| `fork_limit` | 单个 service fork task 数超限 | 1000 | ❌ Phase 2 |

## 三、数据库错误

`shield.db.*` 产生的错误。

| 错误码 | 说明 | retryable | 状态 |
|--------|------|-----------|------|
| `module_unavailable` | database 模块未启用 | 否 | ✅ |
| `database_error` | 未分类的数据库执行失败 | 视具体驱动 | ✅ (代码中使用 `database_error`) |
| `db_query_failed` | 未分类的数据库执行失败 | 视具体驱动 | ❌ Phase 2 (代码使用 `database_error`) |
| `connection_lost` | 数据库连接丢失 | 是 | ❌ Phase 2 |
| `connection_timeout` | 建立连接超时 | 是 | ❌ Phase 2 |
| `query_timeout` | 查询超时 | 是 | ❌ Phase 2 |
| `syntax_error` | SQL 语法错误 | 否 | ❌ Phase 2 |
| `constraint_violation` | 约束违反（唯一键、外键等） | 否 | ❌ Phase 2 |
| `transaction_aborted` | 事务中止 | 是 | ❌ Phase 2 |
| `pool_exhausted` | 连接池耗尽 | 是 | ❌ Phase 2 |

## 四、Redis 错误

`shield.redis.*` / `shield.global()` 等 Redis 相关 API 产生的错误。

| 错误码 | 说明 | retryable | 状态 |
|--------|------|-----------|------|
| `module_unavailable` | redis 模块未启用 | 否 | ✅ |
| `redis_error` | 未分类的 Redis 命令失败 | 视具体驱动 | ✅ (代码中使用 `redis_error`) |
| `redis_command_failed` | 未分类的 Redis 命令失败 | 视具体驱动 | ❌ Phase 2 (代码使用 `redis_error`) |
| `connection_lost` | Redis 连接丢失 | 是 | ❌ Phase 2 |
| `connection_timeout` | 建立连接超时 | 是 | ❌ Phase 2 |
| `command_timeout` | 命令执行超时 | 是 | ❌ Phase 2 |
| `wrong_type` | Redis 类型错误 | 否 | ❌ Phase 2 |
| `pool_exhausted` | 连接池耗尽 | 是 | ❌ Phase 2 |

## 五、网络错误

`shield_net` / `SessionHandle` 相关的错误。

| 错误码 | 说明 | retryable | 状态 |
|--------|------|-----------|------|
| `session_closed` | session 已关闭，handle stale | 否 | ❌ SessionHandle 未实现 |
| `session_send_queue_full` | session 发送队列已满 | 是 | ❌ SessionHandle 未实现 |
| `handshake_timeout` | 握手超时 | 否 | ❌ Phase 2 |
| `decode_error` | 协议解码错误 | 否 | ❌ Phase 2 |
| `connection_limit` | 连接数达到上限 | 是 | ❌ Phase 2 |
| `ip_limit` | 单 IP 连接数达到上限 | 否 | ❌ Phase 2 |

## 六、错误处理建议

### 重试策略

```lua
function M.call_with_retry(target, method, data, max_retries)
    max_retries = max_retries or 3
    local retries = 0

    while retries < max_retries do
        local ok, result = shield.call(target, method, data)
        if ok then
            return true, result
        end

        if result.retryable then
            retries = retries + 1
            if retries < max_retries then
                shield.sleep(100 * retries)  -- 指数退避
            end
        else
            return false, result
        end
    end

    return false, {
        code = "max_retries_exceeded",
        message = "Max retries exceeded",
        source = "runtime",
        retryable = false,
    }
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

业务错误属于 callee 自己的返回契约，不使用 `ok == false` 传递：

```lua
-- Callee
function M.get_player(uid)
    local player = find_player(uid)
    if not player then
        return {
            ok = false,
            code = "PLAYER_NOT_FOUND",
            message = "Player not found",
        }
    end

    return {
        ok = true,
        player = player,
    }
end

-- Caller
local ok, result = shield.call("player", "get_player", uid)
if not ok then
    -- Runtime 错误（本文档定义的错误码）
    shield.log.error("runtime error: " .. result.code)
elseif not result.ok then
    -- 业务错误，由业务自己定义
    shield.log.warn("business error: " .. result.code)
else
    process_player(result.player)
end
```
