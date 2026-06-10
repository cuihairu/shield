# 消息运行时语义

本文档包含 Shield 消息通信相关的运行时语义决策。

## MessageEnvelope

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

## MessagePayload

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

## LuaPack 序列化格式

LuaPack 是 Shield 内置的二进制序列化格式，用于消息编码。

### Wire Format

```
┌─────────────────────────────────────────────────────────┐
│  Header (4 bytes)                                       │
│  - magic: 2 bytes (0x4C, 0x50 = "LP")                  │
│  - version: 1 byte                                      │
│  - flags: 1 byte                                        │
├─────────────────────────────────────────────────────────┤
│  Payload                                                │
│  - type_tag: 1 byte                                     │
│  - data: variable length                                │
└─────────────────────────────────────────────────────────┘
```

字节序：Little-Endian（与 x86/ARM 一致）。

### 类型编码

| 类型 | Tag | 编码 |
|------|-----|------|
| nil | 0x00 | 无数据 |
| false | 0x01 | 无数据 |
| true | 0x02 | 无数据 |
| integer | 0x03 | 8 bytes (int64 LE) |
| number | 0x04 | 8 bytes (double LE) |
| short_string | 0x05 | 1 byte len + data (len < 256) |
| string | 0x06 | 4 bytes len (uint32 LE) + data |
| array | 0x07 | 4 bytes count + elements |
| map | 0x08 | 4 bytes count + key-value pairs |
| service_handle | 0x10 | 4 bytes node + 8 bytes id |
| extension | 0xFF | 2 bytes type_id + 4 bytes len + data |

### 字符串编码

```txt
短字符串 (< 256 bytes):
  [0x05] [len:u8] [data:len bytes]

长字符串 (>= 256 bytes):
  [0x06] [len:u32 LE] [data:len bytes]
```

### 数组编码

```txt
[0x07] [count:u32 LE] [element1] [element2] ...
```

空数组：`[0x07] [0x00 0x00 0x00 0x00]`

### Map 编码

```txt
[0x08] [count:u32 LE] [key1] [value1] [key2] [value2] ...
```

key 必须是 string 或 integer 类型。

### ServiceHandle 编码

```txt
[0x10] [node:u32 LE] [id:u64 LE]
```

用于跨服务传递 handle 引用。

### 扩展类型

```txt
[0xFF] [type_id:u16 LE] [len:u32 LE] [data:len bytes]
```

扩展类型用于自定义编码，如：

- 0x0001: Timestamp (int64)
- 0x0002: UUID (16 bytes)
- 0x0003: Decimal (string representation)

### 嵌套深度限制

默认最大嵌套深度 64 层，超过返回 `encode_failed` 错误。

配置：

```yaml
actors:
  - name: gateway
    script: scripts/gateway.lua
    codec:
      max_nesting_depth: 64       # 最大嵌套深度
      max_string_length: 1048576  # 最大字符串长度 (1MB)
      max_array_length: 1000000   # 最大数组长度
      max_map_entries: 100000     # 最大 map 条目数
```

## shield.send

`shield.send` 是 at-most-once、非阻塞、无 ACK 的投递 API。

```lua
local ok, err = shield.send(target, "kick", uid)
```

返回成功只表示 runtime 接受消息进入投递流程，不表示 receiver 已收到或处理成功。

### 高级选项

`send` 支持可选的第四个参数 `opts`，用于控制背压和 QoS：

```lua
local ok, err = shield.send(target, "event", data, {
  -- 背压策略（当 mailbox 满时）
  backpressure = "drop_oldest",  -- 丢弃最旧消息
  -- backpressure = "drop_newest",  -- 丢弃最新消息（默认）
  -- backpressure = "block",        -- 阻塞直到有空间

  -- QoS 优先级
  priority = "high",  -- "low" | "normal" | "high" | "urgent"

  -- 可靠投递（仅对 send 有效，需要业务 ACK）
  reliable = false,   -- true 时 runtime 会跟踪投递状态
})
```

**背压策略：**

| 策略 | 行为 | 适用场景 |
|------|------|----------|
| `drop_newest` | 丢弃新消息（默认） | 实时性优先，允许丢包 |
| `drop_oldest` | 丢弃旧消息 | 状态更新，只关心最新值 |
| `block` | 阻塞生产者 | 可靠性优先，不能丢消息 |

**优先级：**

| 优先级 | 值 | 说明 |
|--------|-----|------|
| `urgent` | 0 | 最高优先级，立即处理 |
| `high` | 1 | 高优先级 |
| `normal` | 2 | 普通优先级（默认） |
| `low` | 3 | 低优先级 |

优先级影响 mailbox 内的消息排序，高优先级消息优先被取出处理。

### 同步失败错误码

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

### 规则

- `send` 不挂起 coroutine。
- 不自动重试。
- self-send 允许，但不是 reentrant call，而是入队到未来调度点。
- target 可以是 handle 或 name。
- target 是 name 时，每次发送动态 query registry。
- target 是 handle 时，直接按 handle 路由。
- `backpressure = "block"` 会使当前 coroutine 挂起，直到 mailbox 有空间。

可靠处理必须用 `shield.call` 或业务 ACK。

## shield.call 返回格式

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

## call 超时

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

## nested call

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

## 同 service coroutine 调度

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

| 资源 | 默认值 | 说明 |
|------|--------|------|
| `max_mailbox_size` | 10000 | 单个 service 的 mailbox 消息数上限 |
| `max_coroutines_per_service` | 1000 | 单个 service 的最大 coroutine 数 |
| `max_pending_calls_per_service` | 1000 | 单个 service 的待响应 call 数 |
| `max_timers_per_service` | 10000 | 单个 service 的 timer 数 |
| `max_message_size` | 1MB | 单条消息最大体积 |
| `max_fork_tasks_per_service` | 1000 | 单个 service 的 fork task 数 |

超过限制时返回结构化错误，不允许无限增长。

配置示例：

```yaml
actors:
  - name: gateway
    script: scripts/gateway.lua
    limits:                          # 可选覆盖默认值
      max_mailbox_size: 50000
      max_coroutines: 2000
      max_pending_calls: 2000
      max_timers: 20000
```

溢出错误码：

```txt
mailbox_full         # mailbox 达到上限
coroutine_limit      # coroutine 数达到上限
pending_call_limit   # pending call 数达到上限
timer_limit          # timer 数达到上限
message_too_large    # 消息体积超过 max_message_size
fork_limit           # fork task 数达到上限
```

## 错误处理

### 错误类型

Shield 区分两类错误：

| 类型 | 说明 | 处理方式 |
|------|------|----------|
| Runtime 错误 | 框架层面错误（超时、服务不存在等） | 由 shield 框架返回 |
| 业务错误 | 业务逻辑错误 | 由 callee 业务代码返回 |

### Runtime 错误

Runtime 错误通过 `ok == false` 返回：

```lua
local ok, result = shield.call("player", "get_info", uid)
if not ok then
    -- result 是 Error 对象
    shield.log.error("call failed: " .. result.code .. " - " .. result.message)

    -- 根据错误码处理
    if result.code == "timeout" then
        -- 超时处理
    elseif result.code == "service_not_found" then
        -- 服务不存在
    end
end
```

Runtime 错误码：

```txt
invalid_target       # 无效目标（格式错误）
invalid_method       # 无效方法名
encode_failed        # 消息编码失败
message_too_large    # 消息过大
service_not_found    # 服务不存在
service_dead         # 服务已停止
node_offline         # 节点离线
mailbox_full         # 邮箱已满
runtime_stopping     # 运行时正在停止
permission_denied    # 权限不足
timeout              # 调用超时
```

### 业务错误

业务错误由 callee 业务代码定义，通过 `call` 的返回值传递：

```lua
-- Callee 服务
function M.get_player(uid)
    local player = find_player(uid)
    if not player then
        -- 业务错误：返回 nil + 错误信息
        return nil, { code = "PLAYER_NOT_FOUND", message = "Player not found" }
    end
    return player
end

-- Caller 服务
local ok, result, err = shield.call("player", "get_player", uid)
if ok then
    if result == nil then
        -- 业务错误
        shield.log.warn("business error: " .. err.code)
    else
        -- 成功
        process_player(result)
    end
else
    -- Runtime 错误
    shield.log.error("runtime error: " .. result.code)
end
```

### 错误传播

**Call 链中的错误传播：**

```lua
function M.login(uid)
    -- 调用 player 服务
    local ok, profile, err = shield.call("player", "get_profile", uid)
    if not ok then
        -- Runtime 错误，返回给上层
        return nil, err
    end
    if profile == nil then
        -- 业务错误，返回给上层
        return nil, err
    end

    -- 调用 auth 服务
    local ok, token, err = shield.call("auth", "generate_token", profile)
    if not ok then
        return nil, err
    end

    return token
end
```

### 错误恢复

**重试策略（业务层实现）：**

```lua
function M.call_with_retry(target, method, data, max_retries)
    max_retries = max_retries or 3
    local retries = 0

    while retries < max_retries do
        local ok, result = shield.call(target, method, data)
        if ok then
            return result
        end

        -- 只重试超时和服务不存在
        if result.code == "timeout" or result.code == "service_not_found" then
            retries = retries + 1
            if retries < max_retries then
                shield.sleep(100 * retries)  -- 指数退避
            end
        else
            -- 其他错误不重试
            return nil, result
        end
    end

    return nil, { code = "max_retries_exceeded", message = "Max retries exceeded" }
end
```

### 错误日志

建议的错误日志格式：

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
