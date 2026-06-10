# 运行时语义决策稿

本文记录 Shield 重构后的运行时语义决策。当前仍处于设计阶段，本文是编码实现的目标契约，不代表源码已经全部完成。

目标是让后续实现者不需要重新讨论基础语义，可以按本文拆分任务、补测试、替换旧实现。

## 0. 设计原则

- `shield_core` 只承载 Actor/Service 核心语义。
- CAF 是内部机制，不出现在 Lua API 和 public C++ API。
- Lua 是默认业务语言，C++ 负责运行时和少量性能敏感扩展。
- 本地、IPC、cluster 的消息语义必须尽量一致。
- 第一版优先单节点稳定，cluster 和 hot reload 先定边界，不强行实现。

## A1. Target 划分

目标 target：

```txt
shield_base
shield_core
shield_lua
shield_transport
shield_net
shield_ipc
shield_data
shield_config
shield_log
shield_ops
shield_bootstrap
shield
```

职责边界：

| Target | 职责 |
| --- | --- |
| `shield_base` | 基础类型、错误、时间、buffer、result，不依赖 CAF/Lua/网络 |
| `shield_core` | service identity、registry、spawn、send、call、timer、coroutine 状态 |
| `shield_lua` | Lua VM、Lua API binding、LuaPack codec、service module 加载 |
| `shield_transport` | 字节流 framing、codec、加密、压缩、可靠 UDP 等扩展点 |
| `shield_net` | TCP/UDP/WebSocket listener、connection、session |
| `shield_ipc` | 同机进程间通信、IPC heartbeat、远端进程状态 |
| `shield_data` | 原始 DB/Redis 能力 |
| `shield_config` | 配置加载和 schema 校验 |
| `shield_log` | 日志 facade 和 sink |
| `shield_ops` | metrics、health、diagnostics、console、profile |
| `shield_bootstrap` | 组合配置、模块、服务启动顺序 |
| `shield` | 用户入口，提供 `shield::run` |

## A2. CAF 边界

Public headers 禁止出现 CAF 类型。

禁止：

```cpp
caf::actor
caf::event_based_actor*
caf::disposable
caf::message
```

允许：

```txt
src/** 内部实现
未安装的 internal/detail header
CAF adapter 私有实现
```

Public API 使用 Shield 自己的类型：

```cpp
ServiceHandle
ServiceAddress
MessageEnvelope
TimerId
TaskHandle
Result<T>
Error
```

CAF 能力只用于实现 actor spawn、mailbox、request、schedule、remote transport 等机制。

## A3. ServiceHandle

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

## A4. ServiceId 与集群地址

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

## A5. Service Registry 与服务名

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

## A5.1. 心跳与离线清理

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

heartbeat 放在 `shield_ipc` / `shield_cluster`，不进入 `shield_core`。

## A6. shield.spawn

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

## A7. shield.self

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

## A8. MessageEnvelope

`MessageEnvelope` 是 runtime 内部信封，Lua 用户不直接构造。

```cpp
enum class MessageKind {
  Send,
  CallRequest,
  CallResponse,
  System,
};

struct MessageEnvelope {
  MessageKind kind;
  ServiceAddress src;
  ServiceAddress dst;
  uint64_t request_id;
  Deadline deadline;
  TraceContext trace;
  MessageFlags flags;
  MessagePayload payload;
};
```

规则：

- `src` 由 runtime 填充，Lua 不允许伪造。
- `Send` 的 `request_id` 为 0。
- `CallRequest` 和 `CallResponse` 使用非 0 `request_id`。
- `deadline` 本地用 monotonic time。
- 跨节点传输时发送 remaining timeout，不发送本机 monotonic deadline。
- `trace` 用于 ops、profile、慢调用定位。

顺序保证：

```txt
同一个 src -> dst 的本地消息按发送顺序入队。
不同 src 之间不保证全局顺序。
跨节点只保证同一连接内写入顺序。
```

## A9. MessagePayload

core 中 payload 是不可变二进制 buffer。

```cpp
enum class PayloadCodec : uint16_t {
  LuaPack = 1,
  RawBytes = 2,
};

struct MessagePayload {
  PayloadCodec codec;
  uint16_t version;
  ByteBuffer bytes;
};
```

Lua 默认编码：

```txt
LuaRequestPayload {
  method: string
  argc: uint32
  args: LuaValue[]
}
```

支持类型：

| 类型 | 支持 |
| --- | --- |
| `nil` | 是 |
| `boolean` | 是 |
| `integer` | 是 |
| `number` | 是 |
| `string` | 是 |
| `table` | 有限制 |
| `ServiceHandle` | 是，作为扩展类型 |
| `function` | 否 |
| `thread/coroutine` | 否 |
| 普通 userdata | 否 |
| 循环引用 table | 否 |

table 规则：

```txt
array table: 连续整数 key 1..n
map table: string/integer key
禁止: table/function/userdata key
默认最大嵌套深度: 64
```

本地消息也需要序列化，不直接传 Lua 对象指针。优化只能共享 immutable `ByteBuffer`，不能改变语义。

## A10. shield.send

`shield.send` 是 at-most-once、非阻塞、无 ACK 的投递 API。

```lua
local ok, err = shield.send(target, "kick", uid)
```

返回成功只表示 runtime 接受消息进入投递流程，不表示 receiver 已收到或处理成功。

同步失败错误码：

```txt
invalid_target
invalid_method
encode_failed
message_too_large
service_not_found
service_dead
node_offline
mailbox_full
runtime_stopping
permission_denied
```

规则：

- `send` 不挂起 coroutine。
- 不自动重试。
- self-send 允许，但不是 reentrant call，而是入队到未来调度点。
- target 可以是 handle 或 name。
- target 是 name 时，每次发送动态 query registry。
- target 是 handle 时，直接按 handle 路由。

可靠处理必须用 `shield.call` 或业务 ACK。

## A11. shield.call 返回格式

`shield.call` 返回 `ok, ...`。

```lua
local ok, value = shield.call(target, "get_profile", uid)

if not ok then
  local err = value
end
```

成功：

```txt
ok == true
后续返回值是 callee 业务返回值
```

失败：

```txt
ok == false
第二个返回值是 Error
```

业务返回 `nil` 或 `false` 不产生歧义：

```lua
-- callee
function M.check(uid)
  return false, "banned"
end

-- caller
local ok, allowed, reason = shield.call("auth", "check", uid)

-- ok == true
-- allowed == false
-- reason == "banned"
```

response payload 需要保存 `argc`，以保留 trailing nil。

## A12. call 超时

`shield.call` 使用默认超时，不允许默认无限等待。

默认值：

```txt
call_timeout = 5s
```

覆盖超时使用单独 API，避免最后一个业务参数和 options table 歧义：

```lua
local ok, value = shield.call("db.player", "get", uid)
local ok, value = shield.call_timeout(30000, "db.player", "get", uid)
```

规则：

- caller coroutine 挂起。
- 超时后 caller 恢复 `false, Error{ code = "timeout" }`。
- pending call 从 registry 移除。
- callee 不会被自动取消。
- late response 被丢弃，并计入 ops 指标。
- timeout 必须传递到 envelope deadline。

错误对象：

```lua
{
  code = "timeout",
  message = "call timeout",
  source = "runtime",
  retryable = false,
}
```

## A13. nested call

service handler 内允许再次调用 `shield.call`。

```lua
function M.login(uid)
  local ok, profile = shield.call("db.player", "get", uid)
  if not ok then
    return false, profile
  end

  return true, profile
end
```

规则：

- nested call 只挂起当前 coroutine。
- 同一个 service 可以继续处理其他 ready message。
- runtime 不做死锁检测。
- 循环调用依赖 call timeout 释放。
- 需要避免在持有业务锁或临界状态时发起 call。

self-call 允许，但必须按普通消息入队和调度，不能直接递归调用 handler。

## A14. 同 service coroutine 调度

每个 service 拥有一个 Lua VM，VM 内允许多个 Lua coroutine。

规则：

- 同一时间最多一个 OS thread 进入同一个 Lua VM。
- 每条 incoming message 创建或复用一个 coroutine 执行。
- handler 执行到 `call` / `sleep` / await timer 时 yield。
- 当前 coroutine yield 后，该 service 可以处理下一条 ready message。
- response/timer 到达后，把对应 coroutine 放回 ready queue。
- 不提供抢占式调度，只在显式 yield 点切换。

这与 Skynet 类似：业务状态不会多线程并行访问，但会在 yield 点发生重入和交错。

需要限制：

```txt
max_mailbox_size
max_coroutines_per_service
max_pending_calls_per_service
max_timers_per_service
```

超过限制时返回结构化错误，不允许无限增长。

## A15. Lua service module

Lua service 推荐使用 module table。

```lua
local M = {}

function M.on_init(args)
end

function M.ping(value)
  return value
end

function M.on_exit(reason)
end

return M
```

保留 hook：

```txt
on_init(args)
on_exit(reason)
on_error(err, context)
```

业务 method 是 table 上的普通函数：

```lua
function M.get_profile(uid)
  return { uid = uid }
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

## A16. Lua handler 上下文

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

## A17. exit 与 shutdown

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

## A18. timer API

```lua
local id = shield.timer_once(1000, function()
end)

local id = shield.timer(1000, function()
end)

local ok = shield.cancel_timer(id)
```

规则：

- timer callback 在当前 service 的 Lua VM 中执行。
- callback 是 coroutine，可以 `call` / `sleep`。
- timer 归属当前 service。
- service exit 时自动取消 owned timers。
- `TimerId` 是 opaque userdata，不暴露 CAF。

## A19. timer 时间语义

```txt
timer_once(delay_ms)
timer(interval_ms)
```

规则：

- `delay_ms >= 0`。
- `interval_ms > 0`。
- 时间基于 monotonic clock。
- 周期 timer 使用 fixed-delay 语义。

fixed-delay：

```txt
callback 执行完成后，再等待 interval_ms 安排下一次
```

不使用 fixed-rate，避免 callback 慢时堆积。

## A20. timer 错误语义

timer callback 抛错：

- `timer_once` 记录错误后结束。
- `timer` 记录错误并停止该周期 timer。
- 不让未捕获异常反复刷屏。
- 错误进入 `shield_ops` 统计。

如业务希望周期 timer 永不停止，需要自己 `pcall`。

## A21. shield.sleep

```lua
shield.sleep(1000)
```

语义：

- 只挂起当前 coroutine。
- 不阻塞 runtime 线程。
- 基于 timer 实现。
- service exit 时 sleep coroutine 被取消。
- 只能在 service coroutine 中调用。

`sleep(0)` 表示让出当前 coroutine，回到 service ready queue 尾部。

## A22. shield.fork

`shield.fork` 创建当前 service 内的后台 coroutine，不创建新 service。

```lua
local task = shield.fork(function(uid)
  shield.sleep(1000)
  shield.send("player", "tick", uid)
end, uid)
```

规则：

- fork coroutine 与当前 service 共享 Lua VM 和 Lua 状态。
- fork 没有 service id，没有 mailbox，没有 name。
- fork 可以 `call` / `sleep`。
- fork unhandled error 记录日志和 ops 指标，不杀死 service。
- service exit 时自动取消 owned fork tasks。

## A23. TaskHandle

`shield.fork` 返回 `TaskHandle`。

```lua
local task = shield.fork(fn)

task:cancel()
task:status()
task:valid()
```

第一版不提供 `join`，避免引入额外等待图和死锁语义。需要结果时应使用 service `call` 或业务 channel。

## A24. Lua VM 模型

推荐模型：每个 service 一个 Lua VM。

规则：

- service 之间不共享 Lua global。
- service 之间只能通过消息通信。
- 本地消息也序列化，不能共享 Lua table 指针。
- 同一 Lua VM 内多 coroutine 协作式调度。
- 同一时间最多一个 OS thread 进入同一 Lua VM。

代价是内存更高，但边界清晰，利于隔离、热重启、IPC/cluster 一致语义。

可优化点：

- 共享只读 Lua bytecode cache。
- 共享 C++ codec 和 module loader。
- 通过配置限制最大 Lua VM 数量。

## A25. Lua 热更新

第一版不做原地 live patch。

明确不做：

- 不修改正在运行 Lua VM 的函数表。
- 不迁移任意 Lua closure/upvalue。
- 不承诺旧 coroutine 自动切到新代码。

第一版允许：

- 新 spawn 的 service 使用新代码。
- 通过业务流程启动新 service。
- 旧 service draining 后退出。

未来推荐热更新模型是 blue-green service replacement：

```txt
spawn new service
-> init new service
-> switch name binding
-> old service enter draining
-> old service exit after pending work done
```

这需要 registry 支持受控 name handoff，但不应该变成普通 public `replace` API。

## A26. shield_transport

`shield_transport` 处理字节流和 message frame，不处理业务路由。

职责：

- framing。
- codec。
- compression。
- encryption。
- packet validation。
- reliable UDP 扩展点。

不职责：

- service registry。
- Lua handler dispatch。
- gateway 业务逻辑。
- middleware chain。

## A27. shield_net 与 gateway

`shield_net` 管理 listener、connection、session。

业务 gateway 是 Lua service：

```lua
local M = {}

function M.on_connect(session)
end

function M.on_disconnect(session, reason)
end

function M.on_message(session, payload)
end

return M
```

gateway 负责：

- 登录鉴权。
- session 到 player/service 的映射。
- 包路由。
- 限流和业务校验。

core 不提供 middleware framework。

## A28. SessionHandle

Lua 只看到 opaque `SessionHandle`。

```lua
session:id()
session:send(payload)
session:close(reason)
session:remote_addr()
```

规则：

- Lua 不直接操作 socket。
- session send 是 non-blocking。
- backpressure 超限返回错误。
- session 断开后 handle stale，调用返回 `session_closed`。

## A29. 网络背压与限制

网络和 transport 必须有显式限制：

```txt
max_connections
max_frame_size
max_session_send_queue
max_decode_errors
read_idle_timeout
write_idle_timeout
```

超过限制时优先返回错误或主动断开，不允许无界队列。

## A30. shield_data

`shield_data` 提供原始 DB/Redis 能力，不做 ORM。

```lua
local ok, rows = shield.db.query("SELECT * FROM users WHERE id = ?", { uid })
local ok, result = shield.redis.get("player:" .. uid)
```

规则：

- API coroutine-friendly。
- query 不阻塞 runtime 线程。
- 返回 `ok, result_or_error`。
- 支持超时。
- 支持连接池。
- 不做分布式事务。
- 不跨 service 自动共享 transaction。

## A31. data ownership

DB/Redis 连接由 `shield_data` 管理，不由普通 Lua service 直接持有底层连接。

普通 service 只能通过 `shield.db.*` / `shield.redis.*` 调用。

ops 需要暴露：

- pool size。
- in-flight query。
- query latency。
- timeout count。
- reconnect count。

## A32. shield_ops 默认策略

`shield_ops` 不属于 core，但作为官方模块保留。

默认策略：

- 开发环境可本地启用。
- 生产环境默认关闭远程控制入口。
- metrics 可以独立启用。
- profile 必须显式启用。
- console 必须显式启用。

core 只提供可读取的 runtime snapshot 和 counters，不反向依赖 `shield_ops`。

## A33. ops 安全

ops 暴露必须遵守：

- 默认绑定 localhost 或 local admin socket。
- 远程访问必须鉴权。
- 不默认输出完整 payload。
- 对敏感字段做 redaction。
- 控制类 API 需要单独权限。
- profile 和 dump 需要速率限制。

允许暴露：

```txt
service list
registry names
mailbox size
pending calls
timer count
coroutine count
node heartbeat status
slow call summary
```

禁止默认暴露：

```txt
CAF actor handle
完整业务 payload
密钥、token、密码
未鉴权远程 console
```

## A34. cluster 与 shield_ipc

cluster 不进入第一版 `shield_core`。

未来模块：

```txt
shield_ipc      同机多进程通信
shield_cluster  跨机器通信
```

共同语义：

- `NodeId` 唯一。
- handshake 携带 `node_id` 和 `node_epoch`。
- 重复 `NodeId` 必须拒绝连接。
- heartbeat 驱动 online/suspect/offline/removed。
- remote service handle 带 `{node, epoch, service_id}`。
- remote name 是 cache，不是 core registry。

不做：

- core 内置全局服务发现。
- core 内置全局负载均衡。
- public API 暴露 CAF middleman。

## A35. 旧代码处理策略

旧模块处理顺序：

1. 标记所有 public header 中的 CAF 泄漏点。
2. 建立 forbidden include 检查。
3. 抽出 `shield_base` 基础类型。
4. 重建 `ServiceHandle` / `ServiceRegistry`。
5. 替换旧 `service_api` 中 CAF 直出 API。
6. 把 discovery/metrics/health/plugin/DI 等旧模块移出 core 路径。
7. 保留有价值代码时必须归入明确 target。
8. 无 target 归属的旧代码删除或移入实验区。

当前已知需要重点清理：

```txt
include/shield/service/service_api.hpp
include/shield/service/service_handle.hpp
src/service/service_api.cpp
src/actor/actor_starter.cpp
```

## 实现顺序建议

后续编码按以下顺序推进，避免循环依赖。

### M1. 基础类型与 target

- 建立 `shield_base`。
- 定义 `Result<T>`、`Error`、`ByteBuffer`、`TimePoint`。
- 定义 `ServiceId`、`NodeId`、`ServiceAddress`。
- 增加 public header forbidden CAF 检查。

### M2. Service identity 与 registry

- 实现 `ServiceHandle`。
- 实现 `ServiceRegistry`。
- 支持 name reserve/publish/unregister/query。
- 支持 owner exit 自动清理。
- 增加 name conflict、invalid name、stale handle 测试。

### M3. Envelope 与 payload

- 实现 `MessageEnvelope`。
- 实现 `MessagePayload`。
- 实现 LuaPack 编码接口。
- 支持 method、argc、args。
- 增加 nil、false、多返回值、ServiceHandle 编码测试。

### M4. spawn/send/call

- 实现 coroutine-aware `spawn`。
- 实现 non-blocking `send`。
- 实现 `call` pending registry。
- 实现 timeout 和 late response 丢弃。
- 增加 self-send、stale handle、mailbox full 测试。

### M5. Lua service lifecycle

- 实现 module table loader。
- 实现 `on_init` / method dispatch / `on_exit`。
- 实现 `shield.self`、`shield.sender`、`shield.names`。
- 增加 init failure rollback、method_not_found、handler_error 测试。

### M6. timer、sleep、fork

- 实现 `TimerId`。
- 实现 `timer_once`、`timer`、`cancel_timer`。
- 实现 `sleep`。
- 实现 `fork` 和 `TaskHandle`。
- 增加 fixed-delay、timer error stop、service exit auto-cancel 测试。

### M7. net/gateway/data/ops

- 整理 `shield_transport` 和 `shield_net` 边界。
- 实现 gateway Lua callback 草图。
- 保留 data 原始 DB/Redis API。
- 实现 ops snapshot 和本地 diagnostics。

### M8. IPC/cluster 预留

- 不把 cluster 放进 core。
- 为 route key 保留 `node_epoch`。
- 为 ops 增加 node heartbeat 状态模型。
- 后续独立实现 `shield_ipc` 和 `shield_cluster`。
