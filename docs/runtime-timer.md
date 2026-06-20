# 定时器运行时语义

本文档包含 Shield 定时器、sleep 和 fork 相关的运行时语义决策。

当前实现状态：`timer_once/timer/cancel_timer` 已实现，callback 通过 `check_and_fire_each` + `lua_pcall` 包裹执行，错误路由到 `on_error` hook。`shield.sleep` 已实现协程感知（yield + `_resume_after` 定时器 resume）。`shield.fork` 已实现，callback 通过 `lua_pcall` 包裹执行。`shield.call/call_timeout` 已实现协程感知调用 + timeout。详细实现状态以 [重构路线图](roadmap.md) 和 [Lua API 测试用例](lua-api-tests.md) 为准。

## timer API

```lua
local id = shield.timer_once(1000, function()
end)

local id = shield.timer(1000, function()
end)

local ok = shield.cancel_timer(id)
```

规则：

- timer callback 在当前 service 的 Lua VM 中执行，通过 `lua_pcall` 包裹，错误路由到 `on_error` hook。
- callback 当前在 pcall 中执行（非协程），可以调用同步 API；`shield.sleep`/`shield.call` 在 callback 中走同步降级路径。
- timer 归属当前 service。
- service exit 时自动取消 owned timers。
- `TimerId` 是 opaque userdata，不暴露 CAF。

## timer 时间语义

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

## timer 错误语义

timer callback 抛错：

- `timer_once` 记录错误后结束。
- `timer` 记录错误并停止该周期 timer。
- 不让未捕获异常反复刷屏。
- 错误通过 `invoke_error_hook` 路由到 service 的 `on_error(err, context)` hook，`context.type = "timer"`。
- 连续错误达到阈值时触发 `on_panic` 并 `exit("panic")`。
- 错误进入 `shield_ops` 统计（Phase 2）。

如业务希望周期 timer 永不停止，需要自己 `pcall`。

## shield.sleep

```lua
shield.sleep(1000)
```

语义：

- 最终目标是只挂起当前 coroutine；Phase 1 尚未承诺完成该语义。
- 只挂起当前 coroutine。
- 不阻塞 runtime 线程。
- 基于 timer 实现。
- service exit 时 sleep coroutine 被取消。
- 只能在 service coroutine 中调用。

`sleep(0)` 表示让出当前 coroutine，回到 service ready queue 尾部。

## shield.fork

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
- 最终目标中 fork 可以 `call` / `sleep`；Phase 1 尚未承诺完成该语义。
- fork unhandled error 记录日志和 ops 指标，不杀死 service。
- service exit 时自动取消 owned fork tasks。

## TaskHandle

`shield.fork` 返回 `TaskHandle`。

```lua
local task = shield.fork(fn)

task:cancel()
task:status()
task:valid()
```

当前契约不提供 `join`，避免引入额外等待图和死锁语义。需要结果时应使用 service `call` 或业务 channel。
