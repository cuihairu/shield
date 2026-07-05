# Lua API 契约

本文是 Shield 重构后的 Lua 用户 API 契约。运行时语义细节仍由各 `runtime-*.md` 文档展开；当示例或旧源码与本文冲突时，以本文为准。

当前状态：本文冻结 Phase 1 Lua API 契约；源码需要按本文补齐实现和测试。

实现快照：当前源码已跑通单节点 Lua service 路径，包括 `actors` 配置
启动、`on_init/on_exit/on_error/on_panic`、`shield.spawn/exit/self/sender/names/query/register/unregister/now`、
coroutine-aware `shield.call/call_timeout` 与 handler 内 `shield.sleep`、`shield.timer_once/timer/cancel_timer/fork`、
`shield.config`、`shield.log.*`、插件 Lua API（由各插件 `register_lua` 注册到 `shield.<namespace>`，详见 "Plugin-provided APIs"）、
`on_exit` call guard、call timeout（`check_call_timeouts`）、
timer/fork callback `lua_pcall` 包裹（错误路由到 `on_error`）、
TCP gateway listener 到 Lua handler 的 bootstrap 桥接、
HTTP 客户端（`shield.http.*`）以及 `shield_cluster` 的静态 peer/route cache 快照 API。
HTTP 服务端 Lua 路由注册仍是占位入口，尚未接入 bootstrap。
`on_shutdown(ctx)` 和单 VM 内部 `shield.event` 已定义为目标契约，但当前源码尚未实现。

## 设计原则

- Lua service 是普通 Lua module，返回一个 table。
- 不使用 `shield.service("name")` 构造服务对象。
- service handler 只接收业务参数，不隐式注入 `src`、session 或上下文。
- 当前消息上下文通过 `shield.sender()`、`shield.trace()`、`shield.deadline()` 显式读取。
- `shield.send` 非阻塞、无 ACK。
- `shield.call` 挂起当前 Lua coroutine，但不阻塞 runtime 线程。
- Runtime API 返回值统一使用 `ok, result_or_error`，业务返回的 `nil` 和 `false` 不应和 runtime 错误混淆。
- `shield.event` 只用于当前 Lua VM 内部解耦，不跨 service、不跨 VM、不参与 bootstrap lifecycle 编排。
- DB/Redis API 使用插件 namespace + binding 逻辑名调用：`shield.database.mysql("database.default"):query(...)`，不再提供 `shield.db.*` / `shield.redis.*` 全局函数。具体 namespace 与方法见下文 "Plugin-provided APIs"。

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

function M.on_shutdown(ctx)
    shield.log.info(M.name .. " draining: " .. ctx.reason)
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
- `shield.event` 的 listener 表属于当前 service 的 Lua VM；service 退出后随 VM 一起释放。

## Lifecycle Hooks

### on_init(args)

服务创建后、name publish 前调用。

```lua
function M.on_init(args)
    -- args.name: configured or spawned service name, may be nil
    -- args.id: local service id
    -- args.config: actor options/config
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

### on_shutdown(ctx)

运行时进入 graceful shutdown drain 阶段时调用。它是服务级 hook，不是全局事件，也不会通过 `shield.event` 广播。

```lua
function M.on_shutdown(ctx)
    -- ctx.reason: "stopping" | "signal" | "check_config" | ...
    -- ctx.deadline_ms: runtime monotonic deadline
    -- ctx.timeout_ms: 本 service drain 预算
end
```

规则：

- `on_shutdown` 用于停止接收业务入口、flush 内存状态、通知本服务拥有的外部资源进入 drain。
- 运行时先停止 accept/readiness，再按依赖反向顺序调用 `on_shutdown`；依赖图未实现时按 spawn 逆序。
- `on_shutdown` 可以是 coroutine-aware hook，可在 deadline 内使用 `shield.call`、`shield.sleep` 等会挂起的 API。
- 超时或抛错只记录错误并继续关闭流程；随后仍会调用 `on_exit(reason)`。
- 缺失 `on_shutdown` 视为 no-op。
- 不提供 `on_ready` 广播；service ready 定义为 `on_init` 成功并 publish name，application ready 由 bootstrap 在 required actors 启动完且 accept 开启前后判定。

实现状态：目标契约，当前源码尚未实现 `on_shutdown` 调度；当前 `shutdown.timeout.service_drain` 只是配置契约预留。

### on_exit(reason)

服务停止前调用。

```lua
function M.on_exit(reason)
end
```

规则：

- `reason` 是字符串，如 `normal`、`stopping`、`panic`、`timeout`。
- `on_exit` 是 final best-effort 清理，不承担异步 drain。
- 不允许在 `on_exit` 中调用会挂起 coroutine 的 API，例如 `shield.call`、`shield.sleep`。
- 如果需要跨服务 flush 或等待外部 I/O，应放在 `on_shutdown(ctx)`，不要放在 `on_exit(reason)`。

实现快照：`shield.call` / `shield.call_timeout` 在 `on_exit` 上下文中调用时，Lua wrapper 检查 `shield._is_in_exit()` 并立即返回 `false, {code="api_not_allowed_in_exit", message="..."}`。`OnExitCallGuard` 测试覆盖。

### on_error(err, context)

handler、timer、fork task 或本地 `shield.event` listener 抛出错误时调用。

```lua
function M.on_error(err, context)
    shield.log.error(context.type .. ": " .. tostring(err))
end
```

`context` 字段：

| 字段 | 说明 |
| --- | --- |
| `type` | `handler`、`timer`、`fork`、`event` |
| `method` | handler method 名称，非 handler 时为空 |
| `event` | `shield.event` 事件名，仅 `type="event"` 时存在 |
| `trace_id` | 可选 trace id |

`on_error` 不改变服务状态。Phase 1 的 panic 策略固定为：同一 service 连续 handler/timer/fork 未捕获错误达到 `limits.max_errors_before_panic` 时进入 panic；未配置时默认 10。

实现快照：`on_error` / `on_panic` hook 已实现。当 handler 抛错时，`call_service_method_coroutine` 调用 `LuaRuntime::invoke_hook` 触发 service table 上的 `on_error(err, context)`；timer callback 错误通过 `check_and_fire` 回调触发；fork task 错误通过 `pump_once` 触发。连续未捕获错误达到 `kDefaultMaxErrorsBeforePanic`（默认 10）时触发 `on_panic(reason, context)` 并 `exit("panic")`。成功执行后错误计数重置。`OnErrorHookCalledOnHandlerThrow` 测试覆盖。

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
| `module` | actor 类型或脚本别名，由 `actors[].name` 映射 |
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

实现快照：`shield.call` / `shield.call_timeout` 已实现协程感知路径——在 handler 协程中调用时，caller 通过 `_coro_call` → `suspend_for_call` + `coroutine.yield()` 挂起，callee 完成后 `resume_caller` 恢复 caller；主线程调用走 `_sync_call` 同步降级。call timeout 已通过 `pump_once` 中的 `check_call_timeouts` 实现：扫描 `pending_calls` 中超过 `deadline_ms` 的条目，以 `{code="timeout", message="call timeout"}` 恢复 caller。LAPI-005-06 已覆盖。

### Message Context

```lua
local src = shield.sender()
local trace = shield.trace()
local deadline = shield.deadline()
```

规则：

- 只在 message handler coroutine 中有效。
- handler 返回后上下文失效。
- timer callback / fork task 中 `shield.sender()` 返回 `nil`。

实现快照：`shield.sender()`、`shield.trace()`、`shield.deadline()` 均已实现。trace_id 和 deadline_ms 在 send/call 消息中传播，从 caller 的 dispatch context 携带到 callee 的 dispatch context。timer callback / fork task context 中 `shield.sender()` 返回 `nil`，`shield.trace()` 返回 `nil`，`shield.deadline()` 返回 `nil`。

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
- callback 当前通过 `lua_pcall` 执行，不是 coroutine；`shield.sleep` / `shield.call` 在 callback 中走同步降级路径。
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

在 message handler coroutine 中挂起当前 coroutine，不阻塞 runtime thread；在同步调用、timer callback 或 fork task 中走阻塞降级路径。

### shield.fork(fn)

```lua
local task = shield.fork(function()
    shield.send("worker", "done")
end)
```

返回 numeric task id。fork task 属于当前 service，当前通过 `lua_pcall` 执行，不是 coroutine；service exit 时自动取消尚未执行的 task。

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

实现快照：`shield.config` 已实现，key 为扁平字符串匹配（如 `"database.host"`），不支持嵌套路径遍历。返回值自动尝试转换为 boolean/integer/number/string。

## Log API

```lua
shield.log.debug("message")
shield.log.info("message")
shield.log.warn("message")
shield.log.error("message")
```

规则：

- 日志自动注入 service id。
- 参数必须是 string 或可安全 tostring 的值。
- 不允许在日志 API 中执行阻塞 I/O。

实现快照：`shield.log.*` 输出时自动注入当前 service id 前缀（格式：`[service_id] message`）。service name 和 trace id 注入属于 Phase 2 扩展。

## Local Event API

`shield.event` 是单个 Lua service / 单个 Lua VM 内部的同步 observer 工具，用于拆分本 service 内部模块。它不是 runtime event bus，不进入 mailbox，不序列化 payload，不跨 service，不跨 VM，不跨 node，也不承载 lifecycle 编排。

```lua
local off = shield.event.on("inventory.changed", function(payload)
    shield.log.info("changed: " .. payload.item_id)
end)

shield.event.emit("inventory.changed", { item_id = "sword_01" })
off()
```

API：

| API | 说明 |
| --- | --- |
| `shield.event.on(name, fn)` | 注册当前 VM 内 listener，返回 unsubscribe 函数 |
| `shield.event.off(name, fn)` | 移除当前 VM 内 listener |
| `shield.event.emit(name, payload)` | 同步调用当前 VM 内 listener，返回被调用数量 |
| `shield.event.clear(name?)` | 清空指定事件或全部本地 listener |

规则：

- listener 在 `emit` 调用栈内同步执行，顺序为注册顺序。
- listener 抛错时由当前 service 的 `on_error(err, {type="event", event=name})` 处理；其他 listener 继续执行。
- listener 不能用于接收 `application_ready`、`shutdown`、`service_started` 等 runtime lifecycle 事件；这些不是 `shield.event` 的职责。
- 跨 service 解耦使用 `shield.send/call` 或插件队列；跨 node 发布订阅属于可选模块或插件能力。

实现状态：目标契约，当前源码尚未实现 `shield.event`。

## Plugin-provided APIs

数据库、缓存、消息队列、认证、监控、健康检查、匹配等业务能力都由插件提供。插件的 Lua 绑定跟随插件目录，通过 `register_lua` 钩子注册到 `shield.<namespace>`。host 端 `src/lua/lua_api.cpp` 不感知任何具体插件 API。

### 调用形态

每个插件 namespace 是一个 **callable table**：传 `binding` 逻辑名返回绑定到目标 instance 的 proxy。binding 来自主配置 `plugins.bindings`，host 通过 binding 解析到 instance，再向该 instance 取得对应 interface。插件可以为无参调用提供自己的默认 binding 策略，但推荐业务显式传 binding 名。

```lua
-- 默认实例（插件定义的默认 binding，若未配置则返回 module_unavailable）
local db = shield.database.mongodb()

-- 指定 binding 逻辑名
local db_audit = shield.database.mongodb("document.audit")
local db_game  = shield.database.mysql("database.default")

-- 在 proxy 上调用方法
db:insert_one("users", { name = "alice", age = 30 })
db_game:query("SELECT * FROM players WHERE id = ?", { pid })
```

规则：

- **业务代码必须传 binding 逻辑名，不得传 instance_id**（禁止 `shield.database.mysql("db.main")` 这类直接传实例 id 的写法）。binding 是部署可变的逻辑引用，instance_id 是部署细节；设计理由与命名规则见 [插件系统 · 为什么用 binding](plugin-system.md#为什么-lua-访问用-binding-而非-instance-id)。
- API coroutine-friendly。
- 未启用对应插件、binding 不存在或目标实例未启动时返回 `nil, { code = "module_unavailable" }`。
- 同一 package 多实例互不影响；每个 proxy 独立持有连接句柄。
- `proxy` 的生命周期由 Lua GC 管理；连接池归插件 instance 自治，proxy 只持有插件定义的轻量引用或临时连接句柄。

### Database（SQL）— `shield.database.<driver>`

```lua
local db = shield.database.mysql("database.default")

-- SQL CRUD
local ok, rows   = db:query("SELECT * FROM users WHERE id = ?", { uid })
local ok, row    = db:query_one("SELECT * FROM users WHERE id = ?", { uid })
local ok, result = db:execute("UPDATE users SET name = ? WHERE id = ?", { name, uid })

-- 事务
local ok, result = db:transaction(function(tx)
    local ok, updated = tx:execute("UPDATE users SET gold = gold - ? WHERE id = ?", { 10, uid })
    if not ok then return false, updated end
    return true, updated
end)
```

可用 namespace：

| Namespace | 接口 | 说明 |
| --- | --- | --- |
| `shield.database.sqlite` | `shield.database.v1` | 嵌入式 SQLite |
| `shield.database.mysql` | `shield.database.v1` | MySQL X DevAPI |
| `shield.database.postgresql` | `shield.database.v1` | libpq |

### Database（文档）— `shield.database.mongodb`

```lua
local mongo = shield.database.mongodb("document.default")

-- CRUD
mongo:insert_one("users", { _id = "alice", age = 30 })
mongo:insert_many("logs", { { ts = 1 }, { ts = 2 } })

local ok, cursor = mongo:find("users", { age = { ["$gt"] = 18 } })
for _, doc in ipairs(cursor:to_table()) do
    shield.log.info(doc._id)
end

local ok, updated = mongo:update_one("users",
    { _id = "alice" },
    { ["$set"] = { age = 31 } })

local ok, deleted = mongo:delete_one("users", { _id = "alice" })
local ok, count = mongo:count("users", { active = true })

-- 聚合管道
local ok, cursor = mongo:aggregate("users", {
    { ["$match"] = { active = true } },
    { ["$group"] = { _id = "$country", total = { ["$sum"] = 1 } } },
})

-- 索引
mongo:create_index("users", { email = 1 }, { unique = true })

-- 事务（需 MongoDB 4.0+ replica set）
mongo:transaction(function(tx)
    tx:insert_one("orders", { ... })
    tx:update_one("inventory", { sku = "X" }, { ["$inc"] = { qty = -1 } })
end)
```

Lua table 会被 `nlohmann::json` 转为 BSON；查询操作符（`$gt` / `$set` / `$sum` 等）保持 Mongo 原生语义。ObjectId 作为 24 字符十六进制字符串传输。

### Cache — `shield.cache.redis`

```lua
local cache = shield.cache.redis("cache.session")

cache:set("player:" .. uid, json, 3600)
local ok, value = cache:get("player:" .. uid)
cache:del("player:" .. uid)
cache:incr("counter:" .. key)
cache:hset("session:" .. sid, "uid", uid)
local ok, uid = cache:hget("session:" .. sid, "uid")
```

### Queue — `shield.queue.redis`

```lua
local q = shield.queue.redis("queue.events")

q:publish("chat.world", payload)
q:subscribe("chat.world", function(channel, data)
    -- 处理消息
end)
q:unsubscribe("chat.world")
```

### Leaderboard — `shield.leaderboard.redis`

```lua
local lb = shield.leaderboard.redis("leaderboard.default")

lb:create_board("arena_1v1", {
    fields = { { name = "score", dir = "desc" } }
})
lb:set_entry("arena_1v1", "alice", { score = 1500 })
local ok, rank = lb:get_rank("arena_1v1", "alice")
local ok, top = lb:top_n("arena_1v1", 10)
```

### 其他插件

当前已稳定接入 Lua 的插件 namespace，应以各插件文档和实际 `register_lua` 实现为准。

以下 namespace 当前仍属于**规划中**，不要视为已经可用的稳定 Lua API：

| Namespace | 说明 | 当前状态 |
| --- | --- | --- |
| `shield.auth.jwt` | JWT 签发 / 校验 / 刷新 | 规划中，当前 `register_lua` 未实现 |
| `shield.metrics.prometheus` | counter / gauge / histogram | 规划中，当前 `register_lua` 未实现 |
| `shield.health.http` | 注册健康检查、查询状态 | 规划中，当前 `register_lua` 未实现 |
| `shield.matchmaking.elo` | 匹配队列、ELO 评分 | 规划中，当前 `register_lua` 未实现 |

如果后续补齐这些 Lua API，也应优先建立在基础组件与插件接口之上，而不是直接把上层策略塞进 core。相关基础组件方向目前只保留为后置草案，见 [基础组件与运行时适配边界](runtime-primitives.md)。

具体方法签名参考各插件的 `lua/` 封装文件、`plugins/<package>/lua_bindings.cpp` 或对应插件文档中的“规划中”章节。

## Gateway API

Gateway 是 Lua service 模式，不是独立 middleware framework。

```lua
local M = {}

function M.on_connect(session)
end

function M.on_disconnect(session, reason)
end

function M.on_client_message(session, message)
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
- Lua gateway 的长期边界是已解码业务消息；未解码 transport payload 不应进入脚本层。
- 如果 `body.codec = raw`，Lua 收到的可以是字节串，但它属于显式 decode 结果，不是 transport 中间态。
- `SessionHandle` 只在 gateway callback 和 gateway 自身状态中使用，不作为 `shield.send/call` payload 跨服务传递。
- framework 不提供 HTTP middleware chain。

实现快照：Gateway 的 `on_connect/on_disconnect/on_client_message` Lua handler 已定义并可被调用。bootstrap 会为单实例 `actors[].network.tcp` 创建 `TcpListener`，并通过 `LuaGatewayBridge` 将真实 session 事件路由到同名 Lua gateway service。当前桥接已经向 Lua 传入可用的 `SessionHandle` userdata；该 handle 通过内部 session registry 回查 live `shield_net::Session`，因此 `session:send(...)` / `session:close(...)` 已可直接作用于真实连接。

当前 protocol path 的真实语义是：

- `DecodeLocal` 才会进入 `on_client_message(session, message)`
- `body.codec = json` 时，`message` 会作为 Lua table 传入
- `body.codec = raw` 时，`message` 会作为字节串传入
- `ForwardRaw`、`Drop` 和协议错误不会触发 Lua 回调
- `msgpack` 已可作为 structured codec 进入 `DecodeLocal`
- `protobuf`、`sproto`、`xmldef`、`fbs` 这类尚未实现真实 decoder 的 codec 当前不能进入 `DecodeLocal`
- protocol-enabled session 上，`session:send(payload)` 会走固定出站链路：`RouteResolver -> BodyCodec.encode -> Envelope.encode -> socket write`
- `body.codec = raw` 时，`session:send("...")` 发送字节串
- `body.codec = json` / `msgpack` 时，`session:send(...)` 应传业务 table / object；直接传字符串会返回 `protocol_message_required`

## HTTP API

### HTTP 客户端 (shield.http)

基于 libcurl，支持 HTTPS、HTTP/2、连接池、重定向、Cookie、代理、文件上传/下载。

#### 基础请求

```lua
-- GET
local res = shield.http.get("https://api.example.com/health")

-- POST JSON
local res = shield.http.post("https://api.example.com/data", '{"key":"value"}')

-- PUT / DELETE / PATCH
local res = shield.http.put("https://api.example.com/users/1", '{"name":"test"}')
local res = shield.http.delete("https://api.example.com/users/1")
local res = shield.http.patch("https://api.example.com/users/1", '{"name":"new"}')
```

#### 完整请求（所有选项）

```lua
local res = shield.http.request("https://api.example.com/users", {
    method = "POST",
    body = '{"name":"test"}',
    headers = {["X-Custom"] = "value"},
    timeout = 10,
    -- 认证
    auth_bearer = "eyJhbGciOi...",           -- Bearer token
    auth_basic = {user="admin", password="secret"},  -- Basic auth
    -- 代理
    proxy = "http://proxy:8080",
    -- SSL
    verify_ssl = true,
    -- 重试
    retry = 3,
    retry_delay = 1000,
    -- 重定向
    follow_redirects = true,
    max_redirects = 5,
})
```

#### JSON 便捷方法（自动序列化/反序列化）

大部分 HTTP 请求都是 JSON，以下方法自动将 Lua table 序列化为请求体，响应自动解析 `data` 字段：

```lua
-- 最常用：POST JSON（等价于 json_post）
local res = shield.http.json("https://api.example.com/users", {
    name = "test",
    age = 25,
})
-- res.data 自动解析为 Lua table
print(res.data.name)  -- "test"

-- PUT JSON
local res = shield.http.json_put("https://api.example.com/users/1", {
    name = "updated",
})

-- PATCH JSON
local res = shield.http.json_patch("https://api.example.com/users/1", {
    name = "patched",
})

-- 带认证的 JSON 请求
local res = shield.http.json("https://api.example.com/pay", {
    amount = 100,
    currency = "CNY",
}, {
    auth_bearer = "eyJ...",
    timeout = 5,
})
```

返回值额外字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `data` | table | 自动解析的 JSON 响应体（Content-Type 为 JSON 或 body 以 `{`/`[` 开头时） |

#### 文件上传（multipart/form-data）

```lua
local res = shield.http.upload("https://api.example.com/upload", {
    {field_name = "avatar", file_path = "/tmp/photo.png", content_type = "image/png"},
    {field_name = "doc", file_path = "/tmp/report.pdf"},
}, {
    user_id = "12345",
    description = "Profile photo",
}, 60)
```

#### 文件下载

```lua
local res = shield.http.download("https://example.com/data.zip", "/tmp/data.zip", 60)
if res.ok then
    print("Downloaded: " .. res.status)
end
```

#### 表单提交（application/x-www-form-urlencoded）

```lua
local res = shield.http.post_form("https://api.example.com/login", {
    username = "admin",
    password = "secret",
})
```

#### 返回值字段

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `status` | number | HTTP 状态码 |
| `body` | string | 响应体 |
| `ok` | boolean | `status >= 200 && status < 400` |
| `error` | string | 错误信息（成功时为空） |
| `headers` | table | 响应头 |

规则：

- 当前实现同步执行 libcurl 请求；如果在 handler 内调用，会阻塞 Lua worker。异步 HTTP 属于后续工作。
- 支持 HTTP/1.1、HTTP/2、HTTPS（libcurl + OpenSSL/Schannel）。
- 支持重定向、代理、Bearer/Basic auth、自定义 CA、SSL 校验开关、简单重试。连接复用依赖 libcurl easy handle 生命周期，当前未实现显式连接池。
- 适合支付 API、webhook、REST API、文件传输等场景。

实现快照：基于 libcurl 实现 `HttpClient`。`HttpClient::initialize()` 在 `register_full_shield_api` 时自动调用；`proxy`、`auth_basic`、`verify_ssl`、`ca_cert_path`、`retry` 与 `retry_delay` 会传递到 libcurl。

### HTTP 服务端 (shield.httpd)

用于注册路由处理入站 HTTP 请求（管理端点、健康检查、webhook 接收等）。

```lua
-- 注册 GET 路由
shield.httpd.get("/api/health", function(req)
    return { status = "ok" }
end)

-- 注册 POST 路由
shield.httpd.post("/api/data", function(req)
    return { created = true }
end)

-- 注册 PUT / DELETE / PATCH 路由
shield.httpd.put("/api/users/:id", function(req) end)
shield.httpd.delete("/api/users/:id", function(req) end)
shield.httpd.patch("/api/users/:id", function(req) end)
```

规则：

- handler 接收 request table，返回 response table。
- 路由在 bootstrap 阶段注册，运行时不变。
- 不提供 middleware chain。
- 适合管理/运维端点，不适合高并发业务流量。

实现快照：基于 Boost.Beast 的 C++ `HttpServer` 已存在，支持基础路由匹配和 JSON 响应。Lua `shield.httpd.*` 目前只接受注册调用并返回成功，尚未保存 handler 或接入 bootstrap；不要把它视为可用入站 HTTP 服务。

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
| `retryable` | 是否适合业务重试（已实现：timeout=true，其他=false） |
| `detail` | 可选调试信息（Phase 2，当前未填充） |

错误码清单见 [错误码参考](./runtime-errors.md)。

实现快照：Error Object 返回 `code`、`message`、`retryable` 三个字段。`detail` 属于 Phase 2 扩展，当前未填充。timeout 错误 `retryable=true`，其他错误 `retryable=false`。

## 删除的旧 API

以下旧 API 不进入重构目标，不保留兼容层：

- `shield.service("name")`
- 旧 Lua 插件执行模型和旧 `shield.plugin.list/by_type/loaded/capabilities`
- DI/IoC 注入 API
- annotation / condition API
- gateway middleware chain API
- `shield.db.*`、`shield.redis.*` 全局数据 API（包括 `shield.db.query(...)`、`shield.redis.get(...)`）
- `shield.db:query(...)`、`shield.redis:get(...)` 等旧冒号形式
- handler 形如 `on_message(src, msg_type, data)` 的统一入口

说明：插件系统 v1 会重新引入只读 introspection API：`shield.plugin.packages()`、`shield.plugin.instances()`、`shield.plugin.instance(id)`、`shield.plugin.binding(name)`。这些 API 只查询插件 catalog/runtime state，不提供 Lua 插件执行能力，也不暴露 native vtable。
