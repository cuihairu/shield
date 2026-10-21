# 服务运行时语义

本文档包含 Shield 服务相关的运行时语义决策。

## ServiceHandle

`ServiceHandle` 是 opaque service reference，不是 `caf::actor`。

C++：

现实现（`include/shield/lua/lua_runtime.hpp`）：

```cpp
class ServiceHandle {
public:
  explicit ServiceHandle(std::string service_id);
  const std::string& id() const;  // 返回服务名字符串
  uint32_t node() const;          // 单节点 runtime 恒为 0（local）
  bool valid() const;             // 即 service_id 非空

private:
  std::string service_id_;
};
```

Lua：

```lua
local h = shield.self()

h:id()
h:node()
h:valid()
tostring(h)
```

规则：

- `ServiceHandle` 不暴露 CAF。
- `ServiceHandle` 不提供 `caf_handle()`。
- `ServiceHandle` 不提供 `operator caf::actor()`。
- `ServiceHandle` 不拥有 service，只是可路由引用。
- 路由时必须通过 `ServiceRegistry` 二次解析。
- stale handle 可能存在，调用时返回 `service_dead` 或 `node_offline`。
- handle 的唯一身份字段就是服务名字符串 `service_id_`；`id()` 返回该字符串。

## ServiceId 与集群地址

> 实现状态：本节的 `ServiceId = uint64_t`、`ServiceAddress`、`RouteKey` 为预留契约，当前源码中不存在这些类型；现实现中 handle 身份是服务名字符串（见上）。

`ServiceId` 是本地 service id，不把 node id packed 进 public `ServiceId`。

```cpp
using ServiceId = uint64_t;
using NodeId = uint32_t;

struct ServiceAddress {
  NodeId node;
  ServiceId id;
};
```

规则：

- `ServiceId{0}` 为 invalid。
- 本进程内 `ServiceId` 单调递增，不复用。
- 不同节点可以有相同的本地 `ServiceId`。
- 全局地址由 `{node_id, service_id}` 表示。
- cluster/IPC 内部路由需要额外带 `node_epoch`。

内部路由 key：

```cpp
struct RouteKey {
  NodeId node;
  uint64_t node_epoch;
  ServiceId id;
};
```

Skynet 的高位 cluster id、低位 actor id 可以作为思想参考，但 Shield 不把 bit layout 变成 public 契约。未来 wire 层如需紧凑编码，可以内部 packed，不影响 API。

## Service Registry 与服务名

服务名是本地 registry 的唯一别名，不是 service 身份。

规则：

- 一个 name 同时只能绑定一个 service。
- 一个 service 可以拥有 0 个、1 个或多个 name。
- name 只在本 runtime 内唯一。
- cluster 全局命名不进入 `shield_core`。
- service 退出时自动清理其拥有的所有 name。

推荐 API：

```lua
local h, err = shield.spawn("gateway", {
  name = "gateway.main",
})

local h2, err = shield.query("gateway.main")

shield.register("gateway.public")
shield.unregister("gateway.public")
shield.claim("gateway.public")    -- 蓝绿接管已发布的 name
```

名称规则：

```txt
长度: 1-64
允许: a-z A-Z 0-9 _ . -
禁止: 空白、路径分隔符、控制字符
保留前缀: shield.
```

`spawn(..., { name = "x" })` 需要先 reserve name，init 成功后 publish，init 失败或超时则 rollback。

内部 registry 状态：

```txt
reserved  : 名字已占用，service 初始化中，query 不可见
published : service 已 running，query 可见
```

多实例不共享同名：

```lua
shield.spawn("worker", { name = "worker.1" })
shield.spawn("worker", { name = "worker.2" })
shield.spawn("worker_pool", { name = "worker.pool" })
```

round-robin、hash、按房间路由等能力应由显式 pool service 实现，不放进 core registry。

单节点 CAF service runtime 维护 published name 表，支持默认 service name、
`shield.query`、`shield.register`、`shield.unregister`、`shield.claim`、
`shield.names` 和 service exit 自动清理 owned names。spawn 的 name
reserve/publish 状态机与 opaque `ServiceHandle` userdata 已实现；ServiceId
单调分配和 stale handle 语义属于后续契约。

## shield.claim（蓝绿接管）

实现状态：已实现。`shield.claim(name)` 用于蓝绿热更新交接——当前 service 原子接管一个已发布的 name（`lua_service.cpp` 的 `claim_name`，`shield.claim` 已在 `lua_api.cpp` 注册）。

语义：

- 在 registry 锁内完成 owner 迁移：`published_names[name]` 从原 owner 改指当前 service，同时迁移双方 `owned_names` 集合。
- 接管成功后以新 owner 触发 name 变更通知（`notify_name_change`），依赖该通知的路由缓存据此失效。
- 幂等：当前 service 已是该 name 的 owner 时直接返回成功——视为无变更，不触发通知。
- 失败条件：无当前 service 上下文、name 非法或不存在、原 owner 已不运行，返回 `claim_failed`。
- 原 owner 继续运行，由业务自行 drain 后退出；退出清理只 retract 仍归属它的 names（被接管的 name 不受影响）。

只能在自己 service 的 handler 里调用（与 register/unregister 同一调度上下文规则）。详细描述见 [Lua API 契约](lua-api.md) 的 `shield.claim`。

## 心跳与离线清理

本地 service 不需要 heartbeat。本地存活状态由 actor 自身 exit、registry 注销和 handle 失效来界定，清理发生在 service stop/exit 流程中。

远端 IPC/cluster 节点需要 heartbeat 和 lease。

状态机（`cluster_manager` 的 `NodeState`：connecting/online/suspect/offline/removed）：

```txt
connecting -> offline   // 握手在 offline_timeout 内未完成
online -> suspect       // 距上次心跳超过 suspect_timeout_ms
suspect -> offline      // 距上次心跳超过 offline_timeout_ms
suspect/offline -> online  // 收到心跳即恢复
online -> offline       // TCP/IPC 连接明确断开
removed                 // ClusterManager 停止时统一置位（无 offline 后的定时清理）
```

默认值（`cluster.heartbeat_interval_ms` / `cluster.suspect_timeout_ms` / `cluster.offline_timeout_ms` 可配置）：

```txt
heartbeat_interval_ms = 5000
suspect_timeout_ms    = 15000   // 距上次心跳超过该时长进入 suspect
offline_timeout_ms    = 30000   // 距上次心跳超过该时长进入 offline
```

超时阈值按经过时间判定，不是"连续 N 次未收到心跳"；当前没有 `remove_after`（offline 后 60s tombstone）语义。

进入 `offline` 后：

- 新 `send/call` 返回 `node_offline`。
- pending call 返回 `node_offline`。
- 清理该 node 的 remote name cache。
- 清理该 node 的 remote route cache。

heartbeat 放在 `shield_cluster`，不进入 `shield_core`。

## shield.spawn

`shield.spawn` 是同步语义、异步实现。

Lua coroutine 会挂起直到目标 service init 成功或失败，但 runtime 线程不阻塞。

单节点 CAF service runtime 按 YAML `actors[].name` 解析脚本别名，创建独立
Lua VM，调用 `on_init(args)`，成功后发布 service name，失败则返回
`nil, Error`。ServiceHandle userdata、name reserve/publish 状态机和
coroutine-aware 异步 spawn 均已实现：spawn 期间 name 处于 reserved
状态（同名 spawn 失败、query 不可见），`on_init` 成功后 publish；
handler 协程内 `shield.spawn` 挂起 caller，由专用 spawn worker 线程执行
VM 创建与 `on_init`，不阻塞 caller 的 service actor。ServiceId 单调分配
与 stale handle 语义属于后续契约。

```lua
local h, err = shield.spawn("gateway", {
  name = "gateway.main",
  args = { port = 8888 },
  timeout = 10000,
})
```

返回：

```lua
-- success
ServiceHandle, nil

-- failure
nil, Error
```

流程：

```txt
validate opts
-> reserve name
-> allocate ServiceId
-> create actor/service
-> create Lua VM / load module
-> call on_init
-> init success: publish name, return handle
-> init failed/timeout: stop service, rollback name, return error
```

默认 `spawn_timeout` 为 10s，可由 spawn opts 的 `timeout` 字段覆盖；当前没有对应的配置键。

spawn 相关错误码见 [错误码参考](runtime-errors.md#一消息与服务错误)。

## shield.self

`shield.self()` 返回当前 service 的 `ServiceHandle`。

```lua
local me = shield.self()

me:id()
me:node()
me:valid()
```

规则：

- 只能在 service coroutine 中调用。
- 返回值是 immutable userdata。
- 多次调用返回等价 handle。
- handle 身份就是本 service 的服务名字符串：`id()` 返回它，`node()` 恒为 `0`，`valid()` 即非空。
- 当前 service 注册名通过 `shield.names()` 查询。

```lua
local names = shield.names()
```

## Lua service module

Lua service 推荐使用 module table。

```lua
local M = {}

function M.on_init(args)
    -- 初始化逻辑
    M.config = args.config
    M.name = args.name
end

function M.ping(value)
    return value
end

function M.on_shutdown(ctx)
    -- 优雅关闭 drain，允许在 deadline 内做异步 flush
end

function M.on_exit(reason)
    -- 清理逻辑
    shield.log.info("service exiting: " .. reason)
end

return M
```

### 生命周期 hook

**on_init(args)**

服务初始化时调用。无返回值或返回 `true` 表示成功；返回 `false/nil, error` 或抛出异常表示失败。

```lua
function M.on_init(args)
    -- args 结构：
    -- {
    --   name = "service_name",           -- 服务名称
    --   id = "service_name",              -- 服务 ID（当前等于服务名字符串）
    --   config = { ... },                 -- 服务自定义配置（来自 YAML）
    --   args = { ... },                   -- spawn 时传入的参数
    -- }

    -- 初始化数据库连接
    local ok, err = connect_database(args.config.database)
    if not ok then
        return nil, "database connection failed: " .. err
    end

    -- 初始化成功
end
```

初始化失败时：
- `shield.spawn` 返回 `nil, Error`
- 服务不会启动
- 已 reserve 的 name 会 rollback

**on_shutdown(ctx)**

运行时 graceful shutdown drain 阶段调用，用于服务主动停止业务入口并完成有界收尾。

```lua
function M.on_shutdown(ctx)
    -- ctx.reason: 关闭原因，如 "stopping"、"signal"、"check_config"
    -- ctx.deadline_ms: drain 共享截止（业务 Clock，与 shield.now() 同源）
    -- ctx.timeout_ms: 本 service 进入 hook 时的剩余 drain 预算

    M.draining = true

    -- 可以在 deadline 内等待其他服务或 I/O
    local ok, err = shield.call_timeout(
        math.max(1, ctx.deadline_ms - shield.now()),
        "player_store",
        "flush"
    )
    if not ok then
        shield.log.warn("flush failed: " .. err.code)
    end
end
```

规则：

- 缺失 `on_shutdown` 视为 no-op。
- `on_shutdown` 由 runtime 调用，不通过 `shield.event` 广播。
- 调用顺序是依赖反向顺序；当前未实现 actor dependency 图时，按 spawn 逆序。
- 运行时已经停止 accept/readiness，不再接收新的外部入口。
- hook 必须受 `shutdown.timeout.service_drain` 和 `ctx.deadline_ms` 约束。
- 超时、返回失败或抛错只记录并继续关闭；随后仍调用 `on_exit(reason)`。
- shutdown drain 期间不允许 spawn 新 service；`shield.spawn` 会返回失败。已有 mailbox 中的消息（含其他服务的 `shield.call`）仍会正常处理。

`on_shutdown` 与 `prepare_shutdown` 的边界：

- `on_shutdown` 是 runtime hook，由 bootstrap shutdown 流程统一触发。
- `prepare_shutdown` 如果业务需要，可以作为普通 method 显式 `shield.call`，不属于保留 hook。

实现状态：已实现。bootstrap shutdown 在停止 accept/readiness 之后调用 `drain_all`：按 spawn 逆序在各 service 的 actor 线程上运行 `on_shutdown`，整个 drain 阶段共享 `shutdown.timeout.service_drain` 预算（`ctx.timeout_ms` 为进入本 hook 时的剩余预算，预算耗尽后剩余 hook 跳过并记 WARN）；超时被放弃的 hook 不中断，由随后的 `shutdown_all` 统一收尾。

**on_exit(reason)**

服务退出前的 final cleanup hook。它在 graceful drain 完成、超时或被跳过之后调用。

如需异步 flush、跨服务通知或等待外部 I/O，使用 `on_shutdown(ctx)`；`on_exit(reason)` 只做不可挂起的 best-effort 清理。

`reason` 枚举：

| reason | 说明 |
|--------|------|
| `"normal"` | 正常退出（调用 `shield.exit("normal")`，或 `shield.exit()` 缺省） |
| `"panic"` | 致命错误（on_panic 触发） |
| `"timeout"` | 初始化超时 |
| `"stopping"` | 运行时正在停止 |
| `"kicked"` | 预留值：服务退出不产生该 reason（`kicked` 仅作为玩家会话关闭原因存在） |
| `"upgraded"` | 预留值：热更新替换，当前运行时不产生 |

当前运行时实际产生的 reason 为 `normal`（含 `shield.exit` 传入的自定义字符串）、`panic`、`timeout`、`stopping`。

```lua
function M.on_exit(reason)
    -- 清理资源
    if M.db_connection then
        M.db_connection:close()
    end

    -- 注意：on_exit 中不能调用 shield.call（会挂起）
    -- 如需异步收尾，应放在 on_shutdown(ctx)
end
```

普通 service 不提供 `on_ready` hook。service ready 定义为 `on_init` 成功并发布 name；application ready 由 bootstrap 在 required actors 启动完成、网络 accept 开启前后判定。玩家级 ready 属于 `shield_player` 的 `PlayerSession` 状态，见 [玩家生命周期](runtime-player.md)。

**on_error(err, context)**

错误上报钩子，仅用于记录和监控，**不影响服务运行状态**。

```lua
function M.on_error(err, context)
    -- err: 错误信息
    -- context: {
    --   type = "handler" | "timer" | "fork" | "sleep" | "hook" | "client_rpc",
    --   method = "method_name",  -- handler 错误为 method 名；hook 错误为 hook 名；
    --                            -- client_rpc 错误为 route_id；sleep 错误为空
    -- }

    shield.log.error(string.format(
        "uncaught error in %s.%s: %s",
        M.name, context.method or "unknown", err
    ))

    -- 无返回值要求，服务继续运行
end
```

触发条件：

| 来源 | 触发时机 | 服务行为 | 出错单元行为 |
|------|----------|----------|-------------|
| handler | 方法抛出异常 | 继续运行 | 返回错误给 caller |
| timer | callback 抛出异常 | 继续运行 | timer_once 结束，timer 停止 |
| fork | 协程抛出异常 | 继续运行 | fork 协程结束 |
| sleep | sleep 恢复后协程继续执行出错 | 继续运行 | 该协程结束 |
| hook | `on_shutdown` hook 抛出异常 | 继续运行（drain 记录错误后继续） | 本次 hook 以失败结束 |
| client_rpc | 客户端 RPC handler 抛出异常 | 继续运行 | 该次 RPC 以错误结束 |

**on_panic(reason, context)**

致命错误钩子，触发后**服务退出**并进入重启流程。

```lua
function M.on_panic(reason, context)
    -- reason: panic 原因（explicit 触发为业务传入字符串；
    --          阈值触发固定为 "consecutive errors reached limit"）
    -- context: {
    --   type = "explicit" | "handler" | "timer" | "fork" | "sleep" | "hook" | "client_rpc",
    --   method = "method_name",  -- 阈值触发时复用 on_error 的 method 分类
    --                            -- （explicit 触发时为空）；context 无 error 字段
    -- }

    shield.log.error(string.format(
        "PANIC in %s: %s (type=%s)",
        M.name, reason, context.type
    ))

    -- 尝试紧急保存（best-effort，不允许挂起调用）
    if M.data then
        emergency_save(M.data)
    end
end
```

触发条件（当前实现只有两个生产者）：

| context.type | 触发时机 | 说明 |
|-------------|----------|------|
| `"explicit"` | 业务调用 `shield.panic("reason")` | 主动触发，`reason` 为业务传入字符串 |
| `"handler"`/`"timer"`/`"fork"`/`"sleep"`/`"hook"`/`"client_rpc"` | 连续未捕获错误达到阈值（10 次） | 防止错误循环；`type` 复用 `on_error` 的错误来源分类 |

不存在 `"init"`/`"vm"` 生产者：`on_init` 失败只把错误返回给 spawner（`shield.spawn` 返回 `nil, Error`），不触发 `on_panic`。

**连续错误阈值：**

阈值当前硬编码为 10（`kDefaultMaxErrorsBeforePanic`），仅在 handler 成功时清零，没有 60 秒窗口语义。下面的 `panic_threshold` 配置键没有对应解析实现，属预留值：

```yaml
# 未实现（预留）：panic_threshold 键当前无解析，不会生效
actors:
  - name: gateway
    script: scripts/auth.lua
    panic_threshold:
      consecutive_errors: 10      # 预留：当前硬编码 10，不可配置
      window: 60000               # 预留：当前无统计窗口语义
```

**完整错误处理流程：**

```
错误发生
  │
  ├─ handler/timer/fork/sleep/hook/client_rpc 异常
  │   ├─ 调用 on_error（仅上报）
  │   ├─ 检查连续错误计数（阈值 10，handler 成功时清零）
  │   │   ├─ 未达阈值 → 继续运行
  │   │   └─ 达到阈值 → 触发 on_panic → 服务退出
  │   └─ 出错单元独立处理（timer 停止、fork 结束）
  │
  ├─ on_init 失败
  │   └─ 不触发 on_panic：错误返回给 spawner，服务不启动
  │
  └─ 服务退出后
      └─ 重启策略决定是否重启（当前未实现，见「服务重启策略」）
```

### 业务 method

业务 method 是 table 上的普通函数：

```lua
function M.get_profile(uid)
    return { uid = uid, name = "player_" .. uid }
end
```

消息 dispatch：

```txt
payload.method -> M[payload.method](...)
```

找不到 method 时：

```txt
call 返回 method_not_found
send 记录 dead letter / method_not_found 统计
```

### 完整示例

```lua
local M = {}

function M.on_init(args)
    M.name = args.name
    M.config = args.config or {}

    -- 初始化计数器
    M.request_count = 0

    shield.log.info(M.name .. " initialized")
    return true
end

function M.on_exit(reason)
    shield.log.info(string.format(
        "%s exiting (reason: %s, requests: %d)",
        M.name, reason, M.request_count
    ))
end

function M.on_shutdown(ctx)
    M.draining = true
    shield.log.info(string.format(
        "%s draining (reason: %s, timeout_ms: %d)",
        M.name, ctx.reason, ctx.timeout_ms
    ))
end

function M.on_error(err, context)
    -- 仅上报，不影响服务运行
    shield.log.error(string.format(
        "%s error in %s: %s",
        M.name, context.method or "unknown", err
    ))
end

function M.on_panic(reason, context)
    -- 致命错误，服务即将退出
    shield.log.error(string.format(
        "%s PANIC: %s (type=%s)",
        M.name, reason, context.type
    ))
end

-- 业务方法
function M.get_info()
    M.request_count = M.request_count + 1
    return {
        name = M.name,
        request_count = M.request_count,
        uptime = shield.now(),
    }
end

function M.shutdown()
    shield.exit("normal")
end

return M
```

更完整的 Lua API 入口见 [Lua API 契约](./lua-api.md)。

## Lua handler 上下文

handler 默认只接收业务参数，不额外塞入 `src`。

```lua
function M.kick(uid, reason)
end
```

如需当前消息上下文，通过 `ctx` 参数获取：

```lua
function M.handler(ctx, ...)
    local src = ctx.sender
    local trace = ctx.trace
    local deadline = ctx.deadline
end
```

`ctx` 是只读对象，只在 message handler coroutine 中有效。handler 返回后上下文失效。

当前实现状态：`shield.self()` 和 `shield.names()` 已在
单节点 Lua service smoke test 中覆盖。`shield.self()` 返回 opaque
`ServiceHandle` userdata（`id()`/`node()`/`valid()`；`node()` 在单节点
runtime 恒为 `0`）。`ctx.sender` 当前返回本地 service name 字符串，
不是最终的 opaque sender handle。

## exit 与 shutdown

```lua
shield.exit("normal")
```

单服务主动退出语义：

- 标记当前 service 为 stopping。
- 停止接收新的业务消息。
- 取消 owned timers。
- 失败 owned pending calls。
- 注销 owned names。
- 调用 `on_exit(reason)`。
- 释放 Lua VM。

runtime 关闭语义：

```txt
stop accept / readiness
-> call on_shutdown(ctx) with bounded service_drain timeout
-> call on_exit(reason)
-> cancel timers/tasks/coroutines and release VM
```

`on_shutdown` 是 graceful drain hook，允许在 deadline 内挂起和等待；`on_exit` 是 final best-effort 清理 hook，不允许执行会挂起的 `shield.call` 或 `shield.sleep`。

## 服务重启策略

实现状态：**仅校验策略字段，重启机制未实现**。当前配置解析只校验 `restart.policy` 的取值（always/on-failure/never），没有重启执行器：服务退出后不会自动重启，`max_retries`/退避/`ops` 的 `last_restart` 暴露均为预留设计。本节以下内容是目标行为设计稿。

服务异常退出时的重启策略。

### 配置

```yaml
# 实现状态：仅 restart.policy 取值被校验；其余字段无解析、无执行器
actors:
  - name: gateway
    script: scripts/auth.lua
    restart:
      policy: on-failure          # always | on-failure | never（仅取值校验）
      max_retries: 5              # 预留：最大重试次数（0 = 无限）
      initial_delay: 1000         # 预留：初始重试延迟（ms）
      max_delay: 30000            # 预留：最大重试延迟（ms）
      multiplier: 2               # 预留：退避倍数
```

### 策略说明

| 策略 | 行为 |
|------|------|
| `always` | 无论何种原因退出都重启 |
| `on-failure` | 仅异常退出时重启（默认） |
| `never` | 不自动重启 |

### 退避算法

使用指数退避，避免频繁重启：

```txt
delay = min(initial_delay * (multiplier ^ retry_count), max_delay)
```

示例：

```txt
retry 0: 1s
retry 1: 2s
retry 2: 4s
retry 3: 8s
retry 4: 16s
retry 5: 30s (达到 max_delay)
```

### 不重启的情况

以下情况不触发自动重启：

- `shield.exit("normal")` 正常退出
- `shield.exit("stopping")` 运行时停止
- 达到 `max_retries` 上限

### 重启行为

重启时：

1. 释放旧 Lua VM。
2. 等待退避延迟。
3. 创建新 Lua VM。
4. 加载脚本。
5. 调用 `on_init`。
6. 成功则恢复服务，失败则继续重试。

### ops 暴露

```json
{
  "name": "gateway",
  "restart": {
    "policy": "on-failure",
    "retry_count": 2,
    "last_restart": "2026-06-10T12:00:00Z",
    "last_reason": "panic"
  }
}
```

## 服务依赖管理

实现状态：**未实现**。`depends_on` 配置键当前全库无解析，拓扑排序、循环依赖检测、按依赖顺序启动均未实现（本节是设计稿）。运行时依赖按需通过 `shield.call` 处理，这一部分是真实可用的。

服务启动时可以声明依赖关系，确保被依赖的服务先启动。

### 配置

```yaml
# 未实现（预留）：depends_on 键当前无解析
actors:
  - name: gateway
    script: scripts/auth.lua
    depends_on:                # 预留：依赖的服务
      - player_manager
      - server_manager

  - name: player_manager
    script: scripts/player_manager.lua
    depends_on:
      - database
      - redis

  - name: database
    script: scripts/database.lua
    depends_on: []             # 无依赖，最先启动
```

### 启动顺序

```
启动顺序（拓扑排序）：
  1. database (无依赖)
  2. redis (无依赖)
  3. server_manager (无依赖)
  4. player_manager (依赖 database, redis)
  5. gateway (依赖 player_manager, server_manager)
```

### 依赖检查

启动时检查：

1. 循环依赖检测 → 启动失败
2. 依赖服务不存在 → 启动失败
3. 依赖服务启动失败 → 当前服务也失败

```yaml
# 循环依赖示例（启动失败）
actors:
  - name: service_a
    depends_on: [service_b]
  - name: service_b
    depends_on: [service_a]    # 错误：循环依赖
```

### 运行时依赖

运行时依赖（非启动依赖）通过 `shield.call` 处理：

```lua
-- 运行时调用，服务不存在时返回 service_not_found
local ok, result = shield.call("player_manager", "get", uid)
if not ok and result.code == "service_not_found" then
    -- 处理服务不存在的情况
end
```

## 服务资源限制

每个 service 有独立的资源上限，防止无限增长。

| 资源 | 默认值 | 说明 |
|------|--------|------|
| `max_timers_per_service` | 10000 | 单个 service 的 timer 数 |
| `max_message_size` | 1MB | 单条消息最大体积 |
| `max_fork_tasks_per_service` | 1000 | 单个 service 的 fork task 数 |

实现状态：上表三项已实现（均为硬编码常量）。原表中的 `max_coroutines_per_service` 与 `max_pending_calls_per_service` 两行已删除——当前实现没有 coroutine 数与 pending call 数上限检查（`coroutine_limit` 只有错误码映射，没有生产者；pending call 无上限检查）。

超过已实现的限制时返回结构化错误，不允许无限增长。错误码见 [错误码参考](runtime-errors.md#二资源限制错误)。

配置示例：

```yaml
# 未接线（预留）：limits 配置键当前无解析，以下覆盖不会生效
actors:
  - name: gateway
    script: scripts/auth.lua
    limits:                          # 预留：当前无解析
      max_coroutines: 2000
      max_pending_calls: 2000
      max_timers: 20000
```
