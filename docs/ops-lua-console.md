# Lua 诊断控制台设计

本文定义 `shield_ops` 下 Lua 诊断控制台与 Lua 内存观测能力的设计边界。它是 `shield_ops` 的专项设计稿，服务于后续实现 `local admin socket` / `console` / `Lua inspect` 观测面，不代表这些能力已经进入当前最小运行路径。

如果与总纲或运维主文档冲突，以 [架构总纲](architecture.md) 和 [运维运行时语义](runtime-ops.md) 为准。

## 目标

本设计解决两个实际问题：

- 在不侵入业务 Lua API 的前提下，查看某个 service 当前 Lua VM 的运行状态。
- 在开发、测试、预发环境中，快速定位“哪个业务 service 在涨内存”“哪些 table/coroutine/timer 持有了状态”。

设计目标：

- 复用 `shield_ops` 管理平面，不把调试入口做成普通业务 service。
- 以 **每个 service 一个 Lua VM** 的模型为前提，按 service 精确观测。
- 默认提供只读诊断；高风险控制能力必须显式开启。
- 不暴露 CAF、底层线程对象或完整业务 payload。
- 所有调试能力都可关闭，不影响最小 runtime 启动。

## 非目标

本文明确不做：

- 不把任意 Lua 执行作为默认能力。
- 不承诺远程完整 debugger（断点、单步、任意 coroutine 栈回溯、任意局部变量读写）。
- 不把插件连接池、socket buffer、CAF mailbox 内部细节伪装成 Lua 内存数据。
- 不提供业务 Lua API，如 `shield.ops.*` 或 `shield.debug.*`。
- 不把 Telnet 协议本身变成设计中心。目标是“文本控制台可交互”，而不是协议兼容性本身。

## 所属模块与入口

此能力属于 `shield_ops`，不属于 `shield_core`、`shield_lua` 或普通业务 service。

public surface：

```text
local admin socket
console
optional HTTP debug endpoints
```

规则：

- 默认入口是 `local admin socket` 或仅绑定 `127.0.0.1` 的文本控制台。
- 是否允许 `telnet`/`nc` 连接只是传输层实现细节，不改变语义 owner。
- 业务 Lua service 不直接 import、require 或调用此能力。

## 依赖边界

`shield_ops` 对 Lua 诊断控制台只拥有“读取和调度”的职责。

依赖方向：

```text
shield_lua / shield_net / shield_plugin / shield_cluster
  -> runtime snapshot / inspect providers
  -> shield_ops
  -> console / admin socket / HTTP
```

规则：

- `shield_ops` 只能读取只读快照或通过 owner 线程执行受控 inspect 任务。
- `shield_ops` 不能直接持有或跨线程操作 `sol::state` / `lua_State*`。
- `shield_lua` 拥有 Lua VM inspect provider 的语义与实现。
- `shield_ops` 不能通过 console 反向改写 core 语义。

## 基本模型

Shield 当前推荐 Lua VM 模型是每个 service 一个 Lua VM。因此 Lua 诊断控制台的最小观测单位是：

- 一个本地 service
- 对应一个 Lua VM
- 对应一组协程、timer、pending call 和业务 table

这意味着控制台命令默认采用：

```text
lua.inspect <service> ...
lua.eval <service> ...
```

而不是：

```text
lua.inspect global ...
lua.eval all ...
```

跨 service 聚合视图由 `shield_ops` 组合多个 service 的快照给出，但不改变“按 service 归属”的底层模型。

## 能力分层

Lua 诊断能力分三层：

| 层级 | 说明 | 默认策略 |
| --- | --- | --- |
| L1 Snapshot | 只读快照，返回稳定字段 | 开发/测试可启用；生产可局部启用 |
| L2 Inspect | 在目标 VM owner 线程上执行受限 inspect 逻辑 | 开发/测试默认可启；生产默认关闭 |
| L3 Eval | 执行用户提供的 Lua 表达式/代码片段 | 仅开发/预发显式启用；生产默认禁止 |

默认要求：

- 先落地 L1 和 L2。
- L3 必须晚于 L1/L2，且依赖明确的安全开关。

## L1：只读快照

L1 不执行任意 Lua 代码，只暴露运行时可稳定采集的数据。

### 最小字段

每个 service 的 Lua 快照至少应包含：

```json
{
  "service": "player.42",
  "vm": {
    "mode": "per_service",
    "memory_kb": 2048
  },
  "runtime": {
    "mailbox_size": 3,
    "mailbox_dropped": 0,
    "pending_calls": 1,
    "timers": 5,
    "coroutines": 2,
    "pending_tasks": 0
  }
}
```

字段解释：

- `memory_kb`：Lua VM 当前堆占用。语义限定为 Lua allocator 视角，不等于进程 RSS。
- `mailbox_size` / `mailbox_dropped`：service mailbox 当前深度与丢弃数。
- `pending_calls`：当前 coroutine-aware call 未完成数。
- `timers`：该 service 拥有的 timer 数。
- `coroutines`：该 service 当前挂起或活跃 coroutine 数。
- `pending_tasks`：fork/task 队列中属于该 service 的任务数。

### 只读快照要求

- 采集必须低开销、可界定延迟。
- 尽可能通过现有 runtime 计数器或 owner 线程下安全读取获得。
- 不遍历完整业务对象图。
- 不输出完整业务 table 内容。

## L2：受限 inspect

L2 允许在目标 service 的 Lua VM 上执行**受控、预定义**的 inspect 逻辑，但不是任意 Lua 执行。

### 目标

它主要用于回答：

- 哪个 table 占内存最多？
- 哪些 cache / map / registry 在涨？
- 某个 service 当前有哪些 coroutine、timer、pending call？
- 两次采样之间，哪些对象计数变化最大？

### 命令面

建议最小命令集：

```text
lua.inspect <service> summary
lua.inspect <service> memory
lua.inspect <service> refs
lua.inspect <service> coroutines
lua.inspect <service> timers
lua.inspect <service> pending_calls
lua.snapshot <service> [name]
lua.diff <service> <snapshot_a> <snapshot_b>
```

含义：

- `summary`：聚合显示该 service 当前 Lua 诊断摘要。
- `memory`：输出 Lua VM 堆内存及主要 retainers 摘要。
- `refs`：从 service module table 出发，按深度/数量限制遍历对象图，输出 top tables。
- `coroutines`：列出 coroutine 数量、状态摘要、最近 resume 来源等可用信息。
- `timers`：列出 timer 数量、最近到期时间、重复/单次类型摘要。
- `pending_calls`：列出等待返回的 coroutine-aware call 摘要。
- `snapshot`：保存一份 Lua 内存/对象图摘要快照。
- `diff`：比较两份快照，定位增长项。

### 执行模型

L2 inspect 任务必须满足：

- 由 `shield_ops` 请求
- 投递到目标 service 所属 Lua VM 的 owner 线程执行
- 在严格时间预算内完成
- 结果被序列化为只读摘要返回

规则：

- inspect 逻辑不能阻塞业务线程过长时间。
- inspect 结果必须做大小限制。
- inspect 必须支持超时和截断。
- inspect 不应调用业务 handler，也不应发送业务消息。

### 输出限制

默认输出限制：

- 最大遍历节点数
- 最大递归深度
- 最大返回字节数
- 最大字符串截断长度

如果超过限制，结果必须明确标出：

```json
{
  "truncated": true,
  "reason": "node_limit"
}
```

## L3：Lua eval

L3 是高风险能力，仅作为开发和预发环境的显式调试工具。

### 能力定位

`lua.eval` 的目标是快速验证状态，而不是提供远程热修平台。

推荐形态：

```text
lua.eval <service> <expr>
lua.exec <service>
  <multiline code>
.
```

建议优先只支持表达式或受限 chunk，返回单次执行结果。

### 安全策略

`lua.eval` 默认必须满足：

- 仅在 `ops.lua_console.eval_enabled=true` 时可用
- 默认仅允许 `localhost` 或 local admin socket
- 需要独立权限，不与普通只读命令共用最低权限
- 必须有执行超时
- 必须有输出大小上限
- 默认禁止文件系统 / OS / 网络相关危险能力

### 生产环境策略

生产默认：

- `lua.inspect` 可按需启用
- `lua.snapshot` / `lua.diff` 可按需启用
- `lua.eval` 必须关闭

若确需开启，必须满足：

- 仅本地入口
- 有审计日志
- 有显式 session 级鉴权
- 有速率限制
- 有强制超时与输出上限

## Lua 内存语义

Lua 诊断控制台里的“内存”只表示 Lua 运行时可观测的那部分，不应误导为整个进程内存。

### 能观测的内容

- Lua VM 堆占用
- service module table 及其可达 table 数量
- coroutine 数与状态摘要
- timer / pending call / pending task 数量
- 业务 cache、map、registry 等 Lua 对象的估算大小或节点数

### 不能直接等同的内容

- 进程 RSS
- C++ 容器占用
- socket send/recv buffer
- plugin 连接池内存
- OpenSSL / Boost / CAF 内部内存

这些内容应由其他 snapshot/provider 提供，例如：

- plugin pool stats
- listener/session stats
- process-level resource stats

## Snapshot 与 Diff 语义

Lua 内存排查最实用的能力不是单次 dump，而是 snapshot + diff。

### Snapshot

快照记录的是**摘要**，不是完整对象镜像。建议包含：

- 采样时间
- service 名
- Lua VM 总内存
- top tables / caches / registries
- coroutine / timer / pending call 摘要
- 截断标记

### Diff

`lua.diff` 输出关注：

- 总内存变化
- table 数变化
- top growth retainers
- coroutine / timer / pending call 是否同步增长

这样可以区分：

- 真正的 Lua table 泄漏
- timer 未清理
- pending call 堆积
- mailbox/backpressure 引发的“看起来像内存涨”的现象

## 线程与时序要求

这是该设计最关键的约束。

规则：

- 同一时间最多一个 OS thread 进入同一 Lua VM。
- `shield_ops` 不直接读取目标 VM 的内部 Lua 对象。
- inspect/eval 都必须通过 owner 线程串行执行。
- owner 线程如果繁忙，控制台命令可以排队、超时或返回 `busy`。

建议错误码：

- `module_unavailable`
- `service_not_found`
- `service_dead`
- `inspect_timeout`
- `inspect_busy`
- `inspect_truncated`
- `permission_denied`
- `lua_eval_disabled`
- `unsafe_operation_forbidden`

## 输出与脱敏

默认输出应该是摘要，不是原始业务状态转储。

允许默认暴露：

```text
memory total
table counts
top retainers summary
coroutine count
timer count
pending call count
mailbox depth
service names
cluster node status
plugin instance state
```

禁止默认暴露：

```text
完整玩家状态
完整背包/订单/payload 内容
token/password/secret
完整业务请求参数
任意 upvalue/raw registry dump
```

如果 inspect 需要展示示例值，应遵守：

- 默认只展示 key 名和类型
- 字符串值默认截断
- 敏感字段名按规则 redaction

## 审计与限流

所有控制台命令都应进入运维审计日志。至少记录：

- 时间
- 来源（local socket / loopback / remote）
- 操作人或认证主体
- 命令名
- 目标 service
- 是否成功
- 是否超时/截断

以下操作需要更严格审计：

- `lua.eval`
- `lua.exec`
- `lua.snapshot`
- `lua.diff`
- 任何未来的控制类操作

同时应支持：

- 每连接速率限制
- 每命令超时
- 并发 inspect 上限

## 配置草案

建议由 `ops` 配置段拥有该能力：

```yaml
ops:
  enabled: true
  console:
    enabled: true
    bind: "127.0.0.1:9090"
    transport: "text"
    auth:
      type: token
      token: ${OPS_TOKEN}
  lua_console:
    enabled: true
    inspect_enabled: true
    snapshot_enabled: true
    eval_enabled: false
    max_eval_time_ms: 100
    max_output_bytes: 65536
    max_inspect_nodes: 5000
    max_inspect_depth: 8
    max_snapshots_per_service: 8
```

说明：

- `ops.console.*` 负责管理入口本身。
- `ops.lua_console.*` 负责 Lua 诊断能力。
- `eval_enabled` 必须独立于 `inspect_enabled`。

## 命令返回形态

建议同时支持两类输出：

- 人类可读文本
- JSON

例如：

```text
> lua.inspect player.42 summary
service: player.42
lua_memory_kb: 2048
mailbox: 3
pending_calls: 1
timers: 5
coroutines: 2
top_tables:
  M.players  640 KB
  M.cache    512 KB
```

```text
> lua.inspect player.42 summary --json
```

这样后续 HTTP `/ops/*` 可以复用同一组 provider，而不用再定义一套不同语义。

## 分阶段落地建议

### Phase A

- `status`
- `services`
- `service <name>`
- `lua.inspect <service> summary`
- `lua.inspect <service> memory`

目标：先打通 runtime snapshot 和 owner-thread inspect 执行链路。

### Phase B

- `lua.inspect <service> refs`
- `lua.inspect <service> coroutines`
- `lua.inspect <service> timers`
- `lua.snapshot`
- `lua.diff`

目标：解决日常 Lua 泄漏与状态滞留排查。

### Phase C

- `lua.eval`
- `lua.exec`

前提：

- 安全开关、审计、超时、输出限制都已实现
- `allow_os/allow_io` 等 Lua sandbox 语义已真正落地

## 与其他文档的关系

- 运维面总规则见 [运维运行时语义](runtime-ops.md)
- Lua VM 单线程/每 service 模型见 [Lua VM 运行时语义](runtime-lua-vm.md)
- optional module 横向边界见 [官方可选模块契约](optional-modules.md)
- plugin pool 等非 Lua 内存观测见 [Plugin Pool Stats](plugin-pool-stats.md)

## 当前状态

当前状态：**设计稿 / deferred implementation**

这份文档冻结的是方向和边界，不声明当前源码已经实现：

- `shield_ops`
- local admin socket
- Lua inspect provider
- Lua snapshot/diff
- Lua eval

后续实现时，源码、测试和配置校验应向本文与 `runtime-ops.md` 收敛。
