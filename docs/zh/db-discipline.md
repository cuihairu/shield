# DB 使用纪律

> 来源：`docs/architecture-review.md` §4 的【高】风险结论——`shield.database.v1`
> 的同步调用路径阻塞（database.h 全表无 callback/future 参数）。异步入口已
> 落地（[DB 异步入口](db-async-design.md)）：协程派发内 `query` / `query_one` /
> `execute` / `transaction` 挂起调用方协程、SQL 在插件 worker 上执行，service
> 继续处理其他消息。因此**规则 1、2 从硬规则降级为推荐**（阻塞由异步机制
> 本身隔离，不再靠评审挡）；**规则 3–6 仍是硬规则**——同步回退路径（非协程
> 上下文、`async: false`）照旧卡 VM，连接池、超时与事务占用纪律不因异步
> 失效。

## 为什么有这份纪律

Shield 每个 service 是一个单线程执行的 actor + 独占 Lua VM。同步 DB 调用
发生在哪个 service，**该 service 的所有消息就排队等多久**：

```text
db:query(...) 阻塞 3s
  → 该 service 的 VM 卡 3s
  → 期间到达该 service 的所有 call / 客户端 RPC / timer 回调全部延迟 3s
```

- 玩家 service 卡 3s：该玩家掉线感知、操作无响应——可忍。
- 共享 service（world / ranking / chat / matchmaking）卡 3s：**全服卡顿**。
- timer 回调里卡 3s：该 service 的其他 timer 全部顺延。

skynet 生态里同样没有内置 DB，但它的惯例（agent 挂起 + 独立 driver 服务）让
阻塞天然被隔离；Shield 现在用同一形状取得同等效果——协程挂起 + 插件 worker，
由异步入口在机制层完成。上面描述的**同步阻塞**代价仍然真实地存在于回退路径
（非协程上下文、`async: false`）里，那些场合本文纪律就是全部的防线。

## 推荐（异步入口落地后由机制保证，评审不再强制）

1. **DB 调用默认放在专职 DB service 里**（`db.player` / `db.audit` /
   `db.guild` 这类低并发、无客户端热路径的 service）。共享 service 与玩家
   热路径 service 现在**可以**在协程派发内直连 database binding——调用挂起
   而非卡 VM，"一次慢查询卡全服"不再成立。但隔离仍是默认形态：挂起的调用
   链一样受超时与池等待约束，评审不拦截、由业务自担。
2. **业务经 `shield.call` / `shield.call_timeout` 访问 DB service**。异步入口
   去掉了"直连会卡 VM"的硬约束，但玩家在线热路径读写内存状态 + dirty 标记
   周期快照的持久化纪律不变（见 [持久化模式](runtime-persistence.md) 的
   "异步快照保存"）。
## 仍是硬规则

3. **超时必须显式配置，且满足 `call_timeout < query_timeout`**：
   - 插件实例：`query_timeout_ms`（默认 5000）、`acquire_timeout_ms`
     （默认 10000）、`connect_timeout_ms`（默认 5000）。
   - 调用方：`shield.call_timeout(T, ...)` 的 T 要小于查询超时，给错误处理
     留出时间窗口；否则慢查询期间调用方协程挂着，pending_calls 堆积。
   - 异步入口的协程挂起预算同理：实例配置 `call_timeout_ms`（默认
     `query_timeout_ms + 500`）显式 ≥ `query_timeout_ms` 时启动告警——
     调用方先过期，迟到完成无人认领（丢弃 + WARN 日志）。
4. **实例数与 pool 匹配**：DB service 用 `instances: 1` + 插件 `pool_size`
   （默认 4）起步。不要用多个 DB service 实例去堆并发——连接池才是并发闸门；
   也不要给每个玩家 spawn 私有 DB service。
5. **热点读不经 DB**：排行榜、在线列表这类高频读走内存 / global / Redis
   cache 插件，DB 只做冷源与回填。
6. **`transaction` body 内只用 `tx:` 句柄做 SQL**。异步事务挂起期间独占一条
   连接，占用上界 = 单条语句的超时预算，这条"连接持有必须有硬上限"由运行时
   强制：body 内调用池级 `db:query` / `db:query_one` / `db:execute` 或嵌套
   `db:transaction` 立即 raise（评审不用再挡）。body 内调 `shield.call` /
   `shield.sleep` 仍是评审级禁止——那会在持连期间引入无界挂起点。

## 正确形态

专职 DB service（`scripts/db_player.lua`）——唯一允许出现 `db:query` 的地方：

```lua
-- db_player.lua - 专职 DB service：收口 DB 访问,业务经 shield.call 进来
local M = {}

local db

function M.on_init(args)
    -- binding 逻辑名来自 config plugins.bindings,不传 instance id
    db = shield.database.mysql("database.default")
end

-- 读:单行/多行 SELECT
function M.load_profile(uid)
    local ok, rows = db:query(
        "SELECT nickname, level, gold FROM players WHERE uid = ?", { uid })
    if not ok then
        return false, "db_read_failed"
    end
    return true, rows[1]  -- 空结果时为 nil,调用方自行处理
end

-- 写:扣款类操作走事务 + 幂等 key(见 runtime-persistence.md 同步强保存)
function M.debit(uid, amount, op_id)
    local ok, err = db:transaction(function(tx)
        tx:execute(
            "INSERT INTO op_log(op_id, uid, amount) VALUES(?, ?, ?)",
            { op_id, uid, amount })  -- 幂等 key 先落,唯一键冲突=重复请求
        tx:execute(
            "UPDATE wallet SET gold = gold - ? WHERE uid = ? AND gold >= ?",
            { amount, uid, amount })
    end)
    if not ok then
        return false, err or "db_write_failed"
    end
    return true
end

return M
```

业务侧（玩家 service 或任意共享 service）——只有 `shield.call`，没有 binding：

```lua
-- player.lua 在线热路径:内存先行,DB 交给专职 service
function M.buy(ctx, client, request)
    local amount = tonumber(request.amount) or 0
    if amount <= 0 then
        return { code = "bad_amount" }
    end

    -- 写路径(强一致资产):call_timeout < 插件 query_timeout(5s)
    local ok, result, reason = shield.call_timeout(4000,
        "db_player", "debit", client:player_id(), amount, request.op_id)
    if not ok or result == false then
        return { code = reason or "debit_failed" }
    end

    -- 内存立即生效,持久化由异步快照兜底
    M.wallet[client:player_id()] = (M.wallet[client:player_id()] or 0) - amount
    return { code = "ok", gold = M.wallet[client:player_id()] }
end

function M.load_profile(uid)
    -- 冷读:登录/换装等低频路径才允许穿过 DB service
    return shield.call("db_player", "load_profile", uid)
end
```

配置（插件实例 + binding + 显式超时）：

```yaml
plugins:
  instances:
    - id: "mysql-main"
      package: "database.mysql"
      config:
        host: "127.0.0.1"
        port: 3306                  # 经典 MySQL 协议
        database: "game"
        user: "game"
        password: "..."
        query_timeout_ms: 5000      # 单条 SQL 上限
        acquire_timeout_ms: 10000   # 池等待上限
        pool_size: 4
  bindings:
    - name: "database.default"      # 业务侧传这个逻辑名
      instance: "mysql-main"

actors:
  - name: "db_player"               # 专职 DB service:低并发、无客户端监听
    script: "../scripts/db_player.lua"
    instances: 1
```

## 反例（评审直接打回）

```lua
-- 反例 1:共享/热路径 service 直查 DB —— 异步落地后不再是全服卡顿事故,
-- 降级为风格问题:慢查询仍挂起该调用链、仍占池。默认形态还是隔离。
-- (world.lua)
function M.on_enter_room(uid)
    local ok, rows = shield.database.mysql("database.default"):query(
        "SELECT ... FROM players WHERE uid = ?", { uid })  -- △ 评审提醒,不拦截
    ...
end

-- 反例 2:无超时的裸 call —— 慢查询期间调用方协程无限挂起
local ok, profile = shield.call("db_player", "load_profile", uid)  -- 反例：热路径
-- 强一致写路径也应用 call_timeout 盖住 query_timeout

-- 反例 3:每玩家一个 DB service —— 连接数 = 玩家数,打穿连接池
-- config: instances: N × player_service + 每 service 都拿 binding    -- 反例

-- 反例 4:把 conn/tx 对象跨 service 传递 —— 绑定在调用 service 的
-- 同步执行上下文上,跨 service 传等于把阻塞传染给对方              -- 反例
```

## 观测与止损

- `/ops/metrics` 的 `shield_service_pending_calls{service="db_player"}`
  持续上涨 = DB 侧变慢，先看 DB 再扩池；`shield_service_requests_total` 与
  errors 同步看。口径见 [运维运行时语义](runtime-ops.md)。
- 直连方看插件侧 gauge：`shield_plugin_db_pending_async{plugin=...,instance=...}`
  持续上涨 = 异步调用在途堆积；`shield_plugin_db_holding` 长期高位 = 有事务
  长时间持有连接（对照 `pool_size` 看）。
- DB service 变慢只会拖慢显式 `shield.call` 它的业务，不会拖垮共享 service——
  这是隔离纪律买到的爆炸半径控制；直连场合则由协程挂起兜住 service 本体。
- 止损顺序：告警（pending_calls 阈值）→ 业务降级（跳过非关键写，内存兜底）→
  DB 侧处理。

## 何时可以放宽

- 运维/管理端冷路径（GM 工具、离线批处理）在规则 1、2 的推荐之外还可以放宽
  超时要求，但规则 3 的显式超时配置本身仍然强制。
- 异步入口（协程恢复式，[DB 异步入口](db-async-design.md)）已落地，规则 1、2
  按原计划降级为推荐。注意降级的适用范围：**只有协程派发内、且实例未配
  `async: false` 的调用才挂起**；非协程上下文（如 `on_init`）走同步回退，
  照旧阻塞 VM，旧规则全文有效。规则 3–6 不随落地降级。
