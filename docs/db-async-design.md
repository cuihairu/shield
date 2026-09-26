# DB 异步 ABI 立项（协程恢复式异步入口）

> 状态：**立项草案，未实现**。本文是 DB 异步路径的唯一设计依据；在对应
> 里程碑落地前，[DB 使用纪律](db-discipline.md)仍是硬规则。来源：
> [架构评审](architecture-review.md) §4 的【高】风险结论——
> `shield.database.v1` 全同步阻塞，慢查询卡死发起 service 的整个 VM；
> 评审建议两步走的第 (b) 步。

## 问题

每个 service 是单线程 actor + 独占 Lua VM。当前 DB 调用链是**在 actor
线程上同步进驱动**：

```text
Lua db:query(sql, params)
  → 插件 register_lua 注册的 C closure（plugins/*/shield_db_*.cpp）
  → 直接调用原生驱动（mysql_real_query / PQexec / sqlite3_step），阻塞
  → 期间该 service 的所有消息、RPC、timer 回调全部排队
```

关键事实（设计输入，均已核实）：

1. **Lua 面完全绕开 C vtable**。`shield.database.<driver>` 的 proxy 方法
   是插件 `register_lua` 里注册的 C closure，直接进驱动；
   `shield_database_v1`（include/shield/plugin/database.h）在树内
   **没有任何调用方**。因此"给 vtable 加 callback 参数"救不了 Lua 面。
2. 阻塞点在**调用 service 的 actor 线程**；连接池等待
   （`acquire_timeout_ms`，默认 10s）发生在同一线程，池耗尽时阻塞更久。
3. 宿主已有完整且经过生产验证的**协程挂起/恢复机器**
   （`shield.call` 全量复用）：
   - `LuaServiceManager::suspend_for_call`（lua_service.cpp）：锚定调用者
     协程、登记 pending call、CAF 延迟消息驱动超时；
   - `resume_caller` / `complete_call`：**任意线程**可调；经调用方 actor
     mailbox 重排队，yield 窗口竞态（driving-phase guard）已处理；
   - Lua 侧 shim 模式：C++ 原语返回 session id，Lua wrapper
     `coroutine.yield()`，完成时以 JSON 结果恰好 resume 一次。
4. `host_api.lua_post_to_service`（host_api.h）已提供"插件线程 → service
   actor 调度点"的投递原语（destroy_fn 恰好一次语义），queue.redis 消费
   线程已在用同一模式。

## 决策

**采用协程恢复式异步入口**：`db:query` 等 proxy 方法在协程 dispatch 内
**挂起调用者协程**（不阻塞 actor），SQL 在**插件自有的 worker 线程池**
执行，完成经宿主 resume 原语恢复协程、按现有返回形态交付结果。复用
`shield.call` 的挂起/恢复机器端到端，不新造调度。

```text
Lua db:query(sql, params)                （协程 dispatch 内）
  → 插件 Lua shim：调 __db_submit(...)   （插件自带 lua 文件 + C closure）
  → C closure：lua_suspend_current() 登记 session → 任务入插件 worker 池
  → Lua shim：coroutine.yield()          ← actor 线程在此释放，消息继续跑
  …… worker 线程：阻塞执行原生驱动 ……
  → lua_resume_session(session, ok, result_json)   （任意线程可调）
  → 宿主经调用方 actor mailbox 恢复协程 → shim 返回 (ok, rows)
```

### 备选方案（否决理由）

| 方案 | 结论 |
|------|------|
| callback 式 `query_async(sql, params, cb)` | 实现最简（queue.redis 同形），但业务代码回调嵌套，与"handler 顺序写"的协程化基线相悖；不作为主路径 |
| future/句柄轮询 + `shield.await` | 比 yield 多一套句柄生命周期原语，无收益；否决 |
| 给 `shield_database_v1` 加异步 vtable | Lua 面不走 vtable（见问题 1），救不了主路径；且树内无 C++ 消费方。列为非目标 |
| 每 service 独占连接 + 每调用一线程 | 连接数随 service 数膨胀，线程开销不可控；否决 |

### 为什么不是宿主侧统一封装

DB 的参数编解码、连接池、驱动调用都在插件内（core 零数据库代码是既定
边界）。宿主只提供**两个通用原语**，语义与 `shield.call` 完全同构：

| 新增 host_api 条目（结构体尾部追加，ABI 兼容） | 语义 |
|------|------|
| `lua_suspend_current(ctx, timeout_ms, tag) -> session` | 仅在 service dispatch 内合法；包装 `suspend_for_call`；不在协程内返回 0（调用方退回同步路径） |
| `lua_resume_session(ctx, session, ok, result_json)` | **任意线程**可调；包装 `resume_caller`（内部经调用方 actor 重排队）；session 已完成/不存在时返回非零 |

插件侧形态（以 mysql 为例）：namespace 的方法改由**插件自带 Lua 文件**
定义（`host_api.lua_add_path` 已支持），C closure 只做提交：

```lua
-- 插件 lua/query_shim.lua（示意）
function proxy:query(sql, params)
    local session = __db_submit(self.__binding, "query", sql, params)
    if session == 0 then return __db_run_sync(self.__binding, "query", sql, params) end
    local r = table.pack(coroutine.yield())
    if not r[1] then return false, r[2] end
    return true, table.unpack(r, 2, r.n)
end
```

`session == 0`（console eval、on_exit、非协程上下文）时**退回今天的同步
行为**——同步路径继续存在且永远可用，纪律文档对它继续适用。

## 语义契约

### 路径与时序

| 场景 | 行为 |
|------|------|
| 协程 dispatch 内调用 | 挂起协程，SQL 进 worker 池，actor 线程继续处理后续消息；结果按完成顺序恢复（不保序跨调用） |
| 非协程上下文 | 同步执行（现状语义），阻塞调用线程 |
| 调用方超时 | `suspend` 的 timeout 触发 → 协程以 `db_timeout` 类错误恢复；**不是取消**——worker 里阻塞的原生调用继续跑到驱动返回（见下条） |
| 超时后 worker 完成 | resume 返回"session 不存在"→ worker **毒化连接**（销毁不回池，与 vtable transport-error 纪律一致），记 WARNING |
| service 在途退出 | drain 窗口内未完成的 session 按现有 shield.call 退出语义完成（失败恢复）；worker 任务照常跑完，连接正常回收 |
| 停机 | 插件 shutdown 先关 worker 池入队口、drain 在途任务，再关连接池 |

### 事务

`transaction` 的异步形态纳入本设计，附带一条硬规则：**tx body 内除
tx proxy 的 SQL 方法外不允许任何让出**（禁止 `shield.call` / `sleep` /
客户端 RPC）。tx proxy 只暴露 `query` / `execute`，业务拿不到其他可让出
API，连接持有时长 = 各条 SQL 往返之和，由单条超时封顶；`pool.holding`
gauge 暴露持有水位。违反规则的写法在评审层打回（纪律文档同步）。

### 超时配置

沿用既有分层，不新增必配键：worker 内单条 SQL 受 `query_timeout_ms` 约束
（驱动侧，现状）；调用方受 `suspend` timeout 约束，默认取
`query_timeout_ms` + 500ms 余量，特殊场景可经实例 config
`call_timeout_ms` 覆盖。启动期校验：显式配置出现 `调用方 >= 驱动` 时告警
（不拒绝启动——同步路径也受同一对配置约束，现状即如此）。

## 观测

- 复用 `shield.pool.stats.v1`（池容量/使用/等待已有），新增实例级
  `pending_async`（在途异步调用数）与 `pool.holding`（事务连接持有数）
  两个 gauge，进 `/ops/metrics`。
- 复用 `SlowCallRing`（shield.call 慢调用环），tag 用 `db:<driver>:<method>`，
  慢 SQL 与慢 call 同一观测口径。

## 兼容与迁移

- `shield_database_v1` C vtable **原样保留**；host_api 结构体尾部追加两个
  条目，`struct_size` 探测，ABI 版本不 bump（符合 abi.h 稳定性规则）。
- Lua 面方法签名、返回形态（`ok, rows` / `ok, result`）、错误码全部不变；
  既有脚本在协程 dispatch 内从"阻塞 VM"透明变为"挂起协程"——对正确代码
  不可见，收益是该 service 的其他消息不再排队。逃生口：实例 config
  `async: false` 一键退回全同步（灰度/排障用）。
- 落地后 [DB 使用纪律](db-discipline.md)降级：规则 1/2（专职 service 隔离）
  从"硬规则"变"推荐"，规则 3-5（超时、池匹配、热点读不走 DB）对同步与
  异步路径都继续强制。

## 非目标

- 查询取消（driver 全为阻塞 API；超时只保证调用方不再等，见语义契约）。
- C++ 侧异步 vtable（树内无消费方；出现真实 C++ 插件消费方再立项）。
- mongodb 文档接口的异步化（同构可套用本设计，独立排期，不阻塞 SQL 三家）。

## 里程碑

| 阶段 | 交付 | 验收 |
|------|------|------|
| M1 host 原语 | `lua_suspend_current` / `lua_resume_session` + 假插件单测（挂起/恢复/超时/双 completion 拒绝/service 退出） | 全部路径有测试；yield 窗口竞态复用 shield.call 既有守卫不新增 |
| M2 sqlite 端到端 | sqlite 插件 Lua shim + worker 池；actor 级真查询 round-trip 测试（补上现状缺口：当前无任何 db:query 穿 actor 的测试） | 协程内挂起期间同 service 可处理其他消息（时序断言）；超时毒化连接；`async: false` 退回同步 |
| M3 mysql/postgresql | 两驱动接入（连接池跨协程持有的审计） | 池耗尽时挂起而非阻塞 actor（acquire 移入 worker） |
| M4 收口 | tx 异步形态 + `pending_async`/`pool.holding` 指标 + 三文档更新（lua-api.md 契约、runtime-data.md、db-discipline.md 降级） | CI Coverage 全绿；文档口径一致 |

## 未决问题

1. M1 期 spike：插件自带 Lua shim 的加载时机与 `__db_submit` 的注册方式
   （`lua_add_path` + `require` vs register_lua 内联 script）——两者皆可行，
   M2 定稿。
2. `async: false` 灰度键是否需要 service 级粒度（实例级够用的可能性大）。
3. M3 期确认 libmariadb/libpq 是否有值得用的非阻塞 API（有的话 worker 池
   可减线程，但接口语义不变，不阻塞本设计）。

## 相关文档

- [DB 使用纪律](db-discipline.md) —— 同步路径硬规则；本设计落地后降级口径见「兼容与迁移」。
- [数据访问架构](runtime-data.md) —— 插件自治边界、binding 语义、接口分类。
- [插件系统 v1](plugin-system.md) —— host_api、register_lua、ABI 稳定性规则。
- [运行时语义：定时器与任务](runtime-service.md) —— 协程化 dispatch 与 shield.call 机器。
