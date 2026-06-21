# Lua API 契约

本文是 Shield 重构后的 Lua 用户 API 契约。运行时语义细节仍由各 `runtime-*.md` 文档展开；当示例或旧源码与本文冲突时，以本文为准。

当前状态：本文冻结 Phase 1 Lua API 契约；源码需要按本文补齐实现和测试。

实现快照：当前源码已跑通单节点 Lua service 路径，包括 `actors` 配置
启动、`on_init/on_exit/on_error/on_panic`、`shield.spawn/exit/self/sender/names/query/register/unregister/now`、
coroutine-aware `shield.send/call/call_timeout/sleep`、`shield.timer_once/timer/cancel_timer/fork`、
`shield.config`、`shield.log.*`、DB/Redis API（真实驱动入口、mock 降级与错误注入测试）、
`on_exit` call guard、call timeout（`check_call_timeouts`）、
timer/fork callback `lua_pcall` 包裹（错误路由到 `on_error`）、
TCP gateway listener 到 Lua handler 的 bootstrap 桥接、
HTTP 客户端（`shield.http.*`）以及 `shield_cluster` 的静态 peer/route cache 快照 API。
HTTP 服务端 Lua 路由注册仍是占位入口，尚未接入 bootstrap。

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

实现快照：`shield.call` / `shield.call_timeout` 在 `on_exit` 上下文中调用时，Lua wrapper 检查 `shield._is_in_exit()` 并立即返回 `false, {code="api_not_allowed_in_exit", message="..."}`。`OnExitCallGuard` 测试覆盖。

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
- timer/fork coroutine 中 `shield.sender()` 返回 `nil`。

实现快照：`shield.sender()`、`shield.trace()`、`shield.deadline()` 均已实现。trace_id 和 deadline_ms 在 send/call 消息中传播，从 caller 的 dispatch context 携带到 callee 的 dispatch context。timer/fork context 中 `shield.sender()` 返回 `nil`，`shield.trace()` 返回 `nil`，`shield.deadline()` 返回 `nil`。

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
- 配置加载和合并由 C++ bootstrap 完成，Lua 不负责加载 JSON 文件。

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

## Data API

`shield_data` 提供原始 DB/Redis 能力；`shield_lua` 在其上提供轻量
`shield.db.mapper/register_mapper/entity` helper。它们只做显式 SQL 模板、
命名参数绑定和简单实体 CRUD SQL 生成，不是重 ORM，不提供对象图加载、
migration 或跨服务事务。

### DB

```lua
local ok, rows = shield.db.query("SELECT * FROM users WHERE id = ?", { uid })
local ok, row = shield.db.query_one("SELECT * FROM users WHERE id = ?", { uid })
local ok, result = shield.db.execute("UPDATE users SET name = ? WHERE id = ?", { name, uid })
local ok, result = shield.db.transaction(function(tx)
    local ok, updated = tx.execute("UPDATE users SET gold = gold - ? WHERE id = ?", { 10, uid })
    if not ok then return false, updated end
    return true, updated
end)
```

规则：

- API coroutine-friendly。
- 返回形态保持 coroutine-friendly；当前实现仍同步执行，data worker pool 属于后续项。
- SQL 参数必须使用 params 数组传递。
- 未启用 database 时返回 `false, module_unavailable`。
- 连接池耗尽返回 `false, { code = "pool_exhausted", ... }`。
- `shield.db.transaction` 只支持本地单连接事务；callback 返回 `false, reason` 或抛错时 rollback。

### DB Mapper / Entity

```lua
local PlayerMapper = shield.db.mapper({
    SelectProfile = {
        type = "select",
        one = true,
        sql = "SELECT player_id, nickname FROM player WHERE player_id = #{player_id}"
    },
    UpdateNickname = {
        type = "update",
        transaction = "required",
        sql = "UPDATE player SET nickname = #{nickname} WHERE player_id = #{player_id}"
    }
})

local ok, profile = PlayerMapper:SelectProfile({ player_id = uid })
local ok, result = PlayerMapper:UpdateNickname({ player_id = uid, nickname = name })

local Player = shield.db.entity({
    table = "player",
    fields = { "player_id", "nickname", "level" },
    primary_key = "player_id"
})

local ok, inserted = Player:insert({ player_id = uid, nickname = name, level = 1 })
local ok, row = Player:find(uid)
```

规则：

- mapper SQL 只支持 `#{name}` / `#{nested.path}` 参数绑定，编译成 `?` 和 params 数组。
- `${name}` 原样替换和多语句 SQL 会返回 `mapper_unsafe_sql`。
- `transaction="required"` 没有显式 tx 时自动开启本地事务；传入 `tx` 时复用当前事务。
- entity helper 只生成 `insert/update/delete/find`，不做 migration、脏跟踪或关联加载。

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

实现快照：`get/set/del/exists/publish/subscribe` 均已实现。`subscribe` 接受 `(channel, callback)` 参数，callback 在 Redis 回调线程中执行（非 worker 线程）。service exit 时自动取消 owned subscriptions（通过 `cancel_redis_subscriptions`）。

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

实现快照：Gateway 的 `on_connect/on_disconnect/on_client_message` Lua handler 已定义并可被调用（LAPI-009-01~05 测试覆盖）。bootstrap 会为单实例 `actors[].network.tcp` 创建 `TcpListener`，并通过 `LuaGatewayBridge` 将真实 session 事件路由到同名 Lua gateway service。当前桥接传入的是 session 信息 table；`SessionHandle` userdata 已注册并可被 Lua 使用，但真实 `shield_net::Session` 到 userdata 的封装仍属后续集成。

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
- `shield.db:query(...)`、`shield.redis:get(...)` 等冒号形式
- handler 形如 `on_message(src, msg_type, data)` 的统一入口

说明：插件系统 v1 会重新引入只读 introspection API：`shield.plugin.packages()`、`shield.plugin.instances()`、`shield.plugin.instance(id)`、`shield.plugin.binding(name)`。这些 API 只查询插件 catalog/runtime state，不提供 Lua 插件执行能力，也不暴露 native vtable。
