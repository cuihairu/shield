# 服务运行时语义

本文档包含 Shield 服务相关的运行时语义决策。

## ServiceHandle

`ServiceHandle` 是 opaque service reference，不是 `caf::actor`。

C++：

```cpp
class ServiceHandle {
public:
  ServiceId id() const;
  NodeId node() const;
  bool valid() const;

private:
  ServiceAddress address_;
  uint64_t node_epoch_;
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
- name 不是 handle 身份字段，只能作为 debug 信息。

## ServiceId 与集群地址

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

## 心跳与离线清理

本地 service 不需要 heartbeat。本地清理由 service 生命周期事件驱动。

远端 IPC/cluster 节点需要 heartbeat 和 lease。

状态机：

```txt
online -> suspect -> offline -> removed
online -> offline   // TCP/IPC 明确断开时直接进入 offline
offline -> removed  // tombstone 过期后清理
```

默认值：

```txt
heartbeat_interval = 2s
suspect_after      = 3 次未收到心跳，约 6s
offline_after      = 5 次未收到心跳，约 10s
remove_after       = offline 后 60s
```

进入 `offline` 后：

- 新 `send/call` 返回 `node_offline`。
- pending call 返回 `node_offline`。
- 清理该 node 的 remote name cache。
- 清理该 node 的 remote route cache。
- 保留 tombstone 到 `remove_after`。

heartbeat 放在 `shield_cluster`，不进入 `shield_core`。

## shield.spawn

`shield.spawn` 是同步语义、异步实现。

Lua coroutine 会挂起直到目标 service init 成功或失败，但 runtime 线程不阻塞。

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

默认 `spawn_timeout` 为 10s，可由配置和 opts 覆盖。

错误码：

```txt
invalid_module
invalid_name
name_conflict
init_failed
spawn_timeout
runtime_stopping
permission_denied
```

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
- handle 身份不包含 name。
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

function M.on_exit(reason)
    -- 清理逻辑
    shield.log.info("service exiting: " .. reason)
end

return M
```

### 生命周期 hook

**on_init(args)**

服务初始化时调用，必须返回 `true` 表示成功，或 `nil, error` 表示失败。

```lua
function M.on_init(args)
    -- args 结构：
    -- {
    --   name = "service_name",           -- 服务名称
    --   id = 123,                         -- 服务 ID
    --   config = { ... },                 -- 服务自定义配置（来自 YAML）
    --   args = { ... },                   -- spawn 时传入的参数
    -- }

    -- 初始化数据库连接
    local ok, err = connect_database(args.config.database)
    if not ok then
        return nil, "database connection failed: " .. err
    end

    -- 初始化成功
    return true
end
```

初始化失败时：
- `shield.spawn` 返回 `nil, Error`
- 服务不会启动
- 已 reserve 的 name 会 rollback

**on_exit(reason)**

服务退出时调用，用于清理资源。

```lua
function M.on_exit(reason)
    -- reason 值：
    -- "normal"      - 正常退出（调用 shield.exit）
    -- "error"       - 未捕获错误导致退出
    -- "timeout"     - 初始化超时
    -- "stopping"    - 运行时正在停止
    -- "kicked"      - 被其他服务踢出

    -- 清理资源
    if M.db_connection then
        M.db_connection:close()
    end

    -- 注意：on_exit 中不能调用 shield.call（会挂起）
    -- 如需异步收尾，应在业务层提前进入 draining 状态
end
```

**on_error(err, context)**

未捕获错误时调用，用于错误上报。

```lua
function M.on_error(err, context)
    -- err: 错误信息
    -- context: {
    --   type = "handler" | "timer" | "fork",
    --   method = "method_name",  -- 仅 handler 错误
    -- }

    shield.log.error(string.format(
        "uncaught error in %s.%s: %s",
        M.name, context.method or "unknown", err
    ))

    -- 返回 true 表示已处理，服务继续运行
    -- 返回 false 或 nil 表示未处理，服务退出
    return true
end
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

function M.on_error(err, context)
    shield.log.error(string.format(
        "%s error in %s: %s",
        M.name, context.method or "unknown", err
    ))
    return true  -- 继续运行
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

## Lua handler 上下文

handler 默认只接收业务参数，不额外塞入 `src`。

```lua
function M.kick(uid, reason)
end
```

如需当前消息上下文，使用显式 API：

```lua
local src = shield.sender()
local trace = shield.trace()
local deadline = shield.deadline()
```

这些 API 只在 message handler coroutine 中有效。handler 返回后上下文失效。

## exit 与 shutdown

```lua
shield.exit("normal")
```

语义：

- 标记当前 service 为 stopping。
- 停止接收新的业务消息。
- 取消 owned timers。
- 失败 owned pending calls。
- 注销 owned names。
- 调用 `on_exit(reason)`。
- 释放 Lua VM。

`on_exit` 是 best-effort 清理 hook，不允许执行会挂起的 `shield.call`。如需异步收尾，应在业务层提前进入 draining 状态。

## 服务重启策略

服务异常退出时的重启策略。

### 配置

```yaml
actors:
  - name: gateway
    script: scripts/gateway.lua
    restart:
      policy: on-failure          # always | on-failure | never
      max_retries: 5              # 最大重试次数（0 = 无限）
      initial_delay: 1000         # 初始重试延迟（ms）
      max_delay: 30000            # 最大重试延迟（ms）
      multiplier: 2               # 退避倍数
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
    "last_reason": "error"
  }
}
```
