# API 说明

Shield 的 API 仍处于重构设计阶段。本文只说明目标方向和当前约束，不承诺源码已经完整实现。

## 文档口径

- `docs/architecture.md` 是当前重构设计稿。
- `docs/lua-api.md` 是 Lua 用户 API 契约的权威入口。
- `docs/lua-api-tests.md` 是 Lua API 测试用例矩阵。
- `docs/runtime-semantics.md` 是运行时语义和 A1-A35 决策稿。
- `examples/hello_world/` 是用户参考示例，不作为 API 正确性的唯一验收。
- `include/`、`src/` 和 `tests/` 反映当前实现状态，但其中仍包含旧架构遗留模块。
- API 稳定前，不维护按模块展开的完整 API 手册。

## Lua API 契约

完整契约见 [Lua API 契约](./lua-api.md)。摘要如下：

```lua
local handle, err = shield.spawn("gateway", {
  name = "gateway.main",
  args = { port = 8888 },
})

shield.exit()
shield.self()
shield.names()

local ok, err = shield.send(target, "kick", uid)
local ok, value = shield.call(target, "get_profile", uid)
local ok, value = shield.call_timeout(30000, target, "get_profile", uid)

local timer = shield.timer(interval_ms, callback)
local timer = shield.timer_once(delay_ms, callback)
shield.cancel_timer(timer)
shield.sleep(delay_ms)
shield.fork(function() ... end)
shield.now()

local ok, rows = shield.db.query(sql, params)
local ok, result = shield.db.execute(sql, params)
local ok, value = shield.redis.get(key)
local ok = shield.redis.set(key, value, ttl)
local ok = shield.redis.publish(channel, data)
local ok = shield.redis.subscribe(channel, callback)

shield.config(path)
shield.log.info(msg)
shield.log.warn(msg)
shield.log.error(msg)
shield.log.debug(msg)
```

关键约定：

- Lua service 是返回 table 的普通 module，不使用 `shield.service("name")`。
- handler 只接收业务参数，当前 sender 通过 `shield.sender()` 获取。
- `shield.spawn` 返回 ready 后的 `ServiceHandle`，失败返回 `nil, Error`。
- `shield.send` 非阻塞、无 ACK，返回成功只表示 runtime 接受投递。
- `shield.call` 挂起当前 Lua coroutine，但不阻塞 runtime 线程；返回 `ok, ...`，`ok=false` 只表示 runtime/transport/callee exception 级错误。
- `shield.call_timeout` 用于覆盖默认 call timeout，避免最后一个业务参数和 options table 歧义。
- payload 使用 `method + argc + args` 编码，保留多返回值和 trailing nil。
- `ServiceHandle` 不包含 name 身份，name 只是 registry alias。
- `shield_cluster`、`shield_global`、`shield_ops` 属于官方可选模块，不属于最小 API 契约。

完整运行时语义见 [运行时语义决策稿](./runtime-semantics.md)。

## 当前实现提示

当前源码中的 Lua 绑定与本文契约仍存在差距，且仓库里仍保留部分旧架构模块。本文描述的是目标 API 边界，不等于“当前实现已经完整到位”。

实现状态应以 `include/`、`src/`、`tests/` 的实际代码为准；旧的 `shield.service`、冒号式 DB/Redis 调用和 `on_message(src, type, data)` 不再进入重构目标。

## C++ API 目标

目标是提供一个单一入口：

```cpp
#include "shield/shield.hpp"

int main(int argc, char** argv) {
    return shield::run(argc, argv);
}
```

该入口尚未稳定。当前源码仍使用 CLI command 入口。
