# Lua API 契约

本文是 Shield 重构后的 Lua 用户 API 契约。运行时语义细节仍由各 `runtime-*.md` 文档展开；当示例或旧源码与本文冲突时，以本文为准。

当前状态：本文冻结 Phase 1 Lua API 契约；源码需要按本文补齐实现和测试。

实现快照：当前源码已跑通单节点最小 Lua service 路径，包括 YAML actors
启动、`on_init/on_exit`、`shield.spawn/exit/self/sender/names/query/register/unregister/now`、
本地同步 `shield.send/call/call_timeout`、`shield.log.*`、DB/Redis 未启用时的
`module_unavailable` 返回，以及启用配置后的 mock data pool smoke test。
最终 coroutine-aware `call/sleep`、mailbox
调度、timer 取消、opaque ServiceHandle userdata 和完整 LuaPack 编码仍是待实现项。

## 设计原则

- Lua service 是普通 Lua module，返回一个 table。
- 不使用 `shield.service("name")` 构造服务对象。
- service handler 只接收业务参数，不隐式注入 `src`、session 或上下文。
- 当前消息上下文通过 `shield.sender()`、`shield.trace()`、`shield.deadline()` 显式读取。
- `shield.send` 非阻塞、无 ACK。
- `shield.call` 挂起当前 Lua coroutine，但不阻塞 runtime 线程。
- Runtime API 返回值统一使用 `ok, result_or_error`，业务返回的 `nil` 和 `false` 不应和 runtime 错误混淆。
- DB/Redis API 使用点号调用：`shield.db.query(...)`，不使用冒号调用。

## Service Module

标准 service 文件：

```lua
local M = {}

function M.on_init(args)
    M.name = args.name
    M.config = args.config or {}
    shield.log.info(M.name .. " started")
end

function M.ping(value)
    local src = shield.sender()
    shield.send(src, "pong", value)
end

function M.on_exit(reason)
    shield.log.info(M.name .. " exiting: " .. reason)
end

return M
```

加载规则：

- 脚本必须返回 table。
- table 上的普通函数就是可被 `send/call` 分发的 method。
- `on_*` 名称为运行时 hook 或模块 hook 保留，业务 method 禁止使用 `on_` 前缀。
- module 顶层代码只做轻量声明，不执行阻塞 I/O。
- 每个 service 实例有独立 Lua state 或隔离上下文，不能共享可变 Lua 全局业务状态。

## Lifecycle Hooks

### on_init(args)

服务创建后、name publish 前调用。

```lua
function M.on_init(args)
    -- args.name: configured or spawned service name, may be nil
    -- args.id: local service id
    -- args.config: YAML actor options/config
    -- args.args: shield.spawn(..., { args = ... }) payload
end
```

成功：

- 无返回值表示成功。
- `return true` 表示成功。

失败：

- `return false, "reason"` 表示失败。
- `return nil, "reason"` 表示失败。
- 抛出 Lua error 表示失败。

失败后：

- `shield.spawn` 返回 `nil, Error`。
- 已 reserve 的 name rollback。
- service 不进入 running 状态。

### on_exit(reason)

服务停止前调用。

```lua
function M.on_exit(reason)
end
```

规则：

- `reason` 是字符串，如 `normal`、`stopping`、`panic`、`timeout`。
- `on_exit` 是 best-effort 清理。
- 不允许在 `on_exit` 中调用会挂起 coroutine 的 API，例如 `shield.call`、`shield.sleep`。

### on_error(err, context)

handler、timer 或 fork task 抛出错误时调用。

```lua
function M.on_error(err, context)
    shield.log.error(context.type .. ": " .. tostring(err))
end
```

`context` 字段：

| 字段 | 说明 |
| --- | --- |
| `type` | `handler`、`timer`、`fork` |
| `method` | handler method 名称，非 handler 时为空 |
| `trace_id` | 可选 trace id |

`on_error` 不改变服务状态。Phase 1 的 panic 策略固定为：同一 service 连续 handler/timer/fork 未捕获错误达到 `limits.max_errors_before_panic` 时进入 panic；未配置时默认 10。

### on_panic(reason, context)

服务进入不可恢复状态前调用。

```lua
function M.on_panic(reason, context)
    shield.log.error("panic: " .. reason)
end
```

规则：

- 不允许挂起。
- 只能做同步、best-effort 的紧急记录或状态标记。
- 返回值会被忽略。

## Service API

### shield.spawn(module, opts)

```lua
local h, err = shield.spawn("player", {
    name = "player.1001",
    args = { uid = 1001 },
    timeout = 10000,
})
```

参数：

| 参数 | 说明 |
| --- | --- |
| `module` | actor 类型或脚本别名，由 YAML `actors[].name` 映射 |
| `opts.name` | 可选 service name，本 runtime 内唯一 |
| `opts.args` | 传给 `on_init(args).args` 的业务参数 |
| `opts.timeout` | 覆盖 spawn timeout，单位 ms |

返回：

- 成功：`ServiceHandle, nil`
- 失败：`nil, Error`

### shield.exit(reason)

```lua
shield.exit("normal")
```

停止当前 service。`reason` 可省略，默认为 `normal`。

### shield.self()

```lua
local me = shield.self()
```

返回当前 service 的 `ServiceHandle`。只能在 service coroutine 中调用。

### shield.names()

```lua
local names = shield.names()
```

返回当前 service 已发布的本地 name 列表。

### shield.query(name)

```lua
local h, err = shield.query("gateway.main")
```

查询本地 registry。cluster 全局发现不进入 core。

### shield.register(name)

```lua
local ok, err = shield.register("gateway.public")
```

给当前 service 发布一个本地 name。

### shield.unregister(name)

```lua
local ok, err = shield.unregister("gateway.public")
```

注销当前 service 拥有的 name。

## Message API

### shield.send(target, method, ...)

```lua
local ok, err = shield.send("room.1", "join", uid, token)
```

规则：

- `target` 可以是 service name 或 `ServiceHandle`。
- 成功只表示 runtime 接受投递。
- receiver 不存在、mailbox 满、runtime stopping 等返回 `false, Error`。
- `send` 不自动重试。
- self-send 允许，但必须进入未来调度点，不允许 reentrant 执行。

### shield.call(target, method, ...)

```lua
local ok, profile = shield.call("player.1001", "get_profile", uid)
```

规则：

- 挂起当前 Lua coroutine。
- 不阻塞 runtime worker thread。
- 使用默认 call timeout。
- 成功返回：`true, ...callee_returns`
- 失败返回：`false, Error`

业务返回 `nil` 或 `false` 时仍是成功：

```lua
-- callee
function M.check(uid)
    return false, "banned"
end

-- caller
local ok, allowed, reason = shield.call("auth", "check", uid)
-- ok == true, allowed == false
```

### shield.call_timeout(timeout_ms, target, method, ...)

```lua
local ok, result = shield.call_timeout(3000, "db.player", "get", uid)
```

使用单独函数覆盖 timeout，避免最后一个业务参数和 options table 歧义。

### Message Context

```lua
local src = shield.sender()
local trace = shield.trace()
local deadline = shield.deadline()
```

规则：

- 只在 message handler coroutine 中有效。
- handler 返回后上下文失效。
- timer/fork coroutine 中 `shield.sender()` 返回 `nil`。

## Timer API

### shield.timer_once(delay_ms, callback)

```lua
local id = shield.timer_once(1000, function()
    shield.send("room.1", "tick")
end)
```

### shield.timer(interval_ms, callback)

```lua
local id = shield.timer(1000, function()
    shield.log.debug("heartbeat")
end)
```

规则：

- fixed-delay：callback 结束后再安排下一次。
- callback 抛错时触发 `on_error`。
- service exit 自动取消 owned timers。

### shield.cancel_timer(timer_id)

```lua
local ok, err = shield.cancel_timer(id)
```

### shield.sleep(delay_ms)

```lua
shield.sleep(100)
```

挂起当前 coroutine，不阻塞 runtime thread。

### shield.fork(fn)

```lua
local task = shield.fork(function()
    shield.sleep(100)
    shield.send("worker", "done")
end)
```

返回 `TaskHandle`。fork task 属于当前 service，service exit 时自动取消。

## Time API

```lua
local ms = shield.now()
```

`shield.now()` 返回 runtime monotonic milliseconds，用于相对时间和 timeout，不用于持久化业务时间戳。

## Config API

```lua
local host = shield.config("database.host", "localhost")
```

规则：

- 读取 bootstrap 后的配置快照。
- 默认只读。
- 找不到 key 时返回第二个参数作为默认值；没有默认值则返回 `nil`。
- 配置加载和合并由 C++ bootstrap 完成，Lua 不负责加载 YAML 文件。

## Log API

```lua
shield.log.debug("message")
shield.log.info("message")
shield.log.warn("message")
shield.log.error("message")
```

规则：

- 日志自动注入 service id、service name、trace id。
- 参数必须是 string 或可安全 tostring 的值。
- 不允许在日志 API 中执行阻塞 I/O。

## Data API

`shield_data` 提供原始 DB/Redis 能力，不提供 ORM、mapper 或跨服务事务。

### DB

```lua
local ok, rows = shield.db.query("SELECT * FROM users WHERE id = ?", { uid })
local ok, row = shield.db.query_one("SELECT * FROM users WHERE id = ?", { uid })
local ok, result = shield.db.execute("UPDATE users SET name = ? WHERE id = ?", { name, uid })
```

规则：

- API coroutine-friendly。
- 查询不阻塞 runtime worker thread。
- SQL 参数必须使用 params 数组传递。
- 未启用 database 时返回 `false, module_unavailable`。

### Redis

```lua
local ok, value = shield.redis.get("player:" .. uid)
local ok, err = shield.redis.set("player:" .. uid, value, 3600)
local ok, receivers = shield.redis.publish("chat.world", payload)
local ok, sub = shield.redis.subscribe("chat.world", function(channel, payload)
end)
```

规则：

- 未启用 Redis 时返回 `false, module_unavailable`。
- subscribe callback 属于当前 service。
- service exit 时自动取消 owned subscriptions。

## Gateway API

Gateway 是 Lua service 模式，不是独立 middleware framework。

```lua
local M = {}

function M.on_connect(session)
end

function M.on_disconnect(session, reason)
end

function M.on_client_message(session, payload)
end

return M
```

`SessionHandle`：

```lua
session:id()
session:send(payload)
session:close("reason")
session:remote_addr()
```

规则：

- Lua 不直接操作 socket。
- `session:send` 非阻塞，队列满返回错误。
- 连接鉴权、限流、路由策略写在 Lua gateway service 中。
- `SessionHandle` 只在 gateway callback 和 gateway 自身状态中使用，不作为 `shield.send/call` payload 跨服务传递。
- framework 不提供 HTTP middleware chain。

## Error Object

运行时错误统一返回只读 table：

```lua
if not ok then
    shield.log.warn(err.code .. ": " .. err.message)
end
```

字段：

| 字段 | 说明 |
| --- | --- |
| `code` | 稳定错误码字符串 |
| `message` | 面向日志的错误说明 |
| `retryable` | 是否适合业务重试 |
| `detail` | 可选调试信息 |

错误码清单见 [错误码参考](./runtime-errors.md)。

## 删除的旧 API

以下旧 API 不进入重构目标，不保留兼容层：

- `shield.service("name")`
- `shield.plugin.*`
- DI/IoC 注入 API
- annotation / condition API
- gateway middleware chain API
- `shield.db:query(...)`、`shield.redis:get(...)` 等冒号形式
- handler 形如 `on_message(src, msg_type, data)` 的统一入口
