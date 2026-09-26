# DB 使用纪律（同步 ABI 下）

> 本文是**硬规则**，不是风格建议。来源：`docs/architecture-review.md` §4 的
> 【高】风险结论——`shield.database.v1` ABI 全同步阻塞（database.h 全表无
> callback/future 参数），异步入口是 Phase 2 立项；在异步入口落地之前，本文
> 规则把"一次慢查询卡全服"这类事故挡在代码评审层。

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
阻塞天然被隔离；Shield 在异步 ABI 落地前，靠的就是本文的**专职 service 隔离**
纪律来取得同等效果。

## 硬规则

1. **DB 调用只允许出现在专职 DB service 里**（`db.player` / `db.audit` /
   `db.guild` 这类低并发、无客户端热路径的 service）。共享 service 和玩家
   在线热路径 service 禁止直接持有 database binding。
2. **业务经 `shield.call` / `shield.call_timeout` 访问 DB service**，不直连。
   玩家 service 的在线热路径读写内存状态，持久化交给 dirty 标记 + 周期快照
   （见 [持久化模式](runtime-persistence.md) 的"异步快照保存"）。
3. **超时必须显式配置，且满足 `call_timeout < query_timeout`**：
   - 插件实例：`query_timeout_ms`（默认 5000）、`acquire_timeout_ms`
     （默认 10000）、`connect_timeout_ms`（默认 5000）。
   - 调用方：`shield.call_timeout(T, ...)` 的 T 要小于查询超时，给错误处理
     留出时间窗口；否则慢查询期间调用方协程挂着，pending_calls 堆积。
4. **实例数与 pool 匹配**：DB service 用 `instances: 1` + 插件 `pool_size`
   （默认 4）起步。不要用多个 DB service 实例去堆并发——连接池才是并发闸门；
   也不要给每个玩家 spawn 私有 DB service。
5. **热点读不经 DB**：排行榜、在线列表这类高频读走内存 / global / Redis
   cache 插件，DB 只做冷源与回填。

## 正确形态

专职 DB service（`scripts/db_player.lua`）——唯一允许出现 `db:query` 的地方：

```lua
-- db_player.lua - 专职 DB service：隔离同步阻塞,业务经 shield.call 访问
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
        port: 33060                 # X Protocol
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
-- 反例 1:共享/热路径 service 直查 DB —— 一次慢查询 = 全服卡顿
-- (world.lua)
function M.on_enter_room(uid)
    local ok, rows = shield.database.mysql("database.default"):query(
        "SELECT ... FROM players WHERE uid = ?", { uid })  -- ✗
    ...
end

-- 反例 2:无超时的裸 call —— 慢查询期间调用方协程无限挂起
local ok, profile = shield.call("db_player", "load_profile", uid)  -- ✗ 热路径
-- 强一致写路径也应用 call_timeout 盖住 query_timeout

-- 反例 3:每玩家一个 DB service —— 连接数 = 玩家数,打穿连接池
-- config: instances: N × player_service + 每 service 都拿 binding    -- ✗

-- 反例 4:把 conn/tx 对象跨 service 传递 —— 绑定在调用 service 的
-- 同步执行上下文上,跨 service 传等于把阻塞传染给对方              -- ✗
```

## 观测与止损

- `/ops/metrics` 的 `shield_service_pending_calls{service="db_player"}`
  持续上涨 = DB 侧变慢，先看 DB 再扩池；`shield_service_requests_total` 与
  errors 同步看。口径见 [运维运行时语义](runtime-ops.md)。
- DB service 变慢只会拖慢显式 `shield.call` 它的业务，不会拖垮共享 service——
  这就是隔离纪律买到的爆炸半径控制。
- 止损顺序：告警（pending_calls 阈值）→ 业务降级（跳过非关键写，内存兜底）→
  DB 侧处理。

## 何时可以放宽

- 运维/管理端冷路径（GM 工具、离线批处理）可以放宽规则 2 的超时要求，但
  规则 1（专职 service）与超时配置本身仍然强制。
- Phase 2 异步 ABI（callback/future + 协程恢复）落地后，本文规则降级为
  "推荐"：届时阻塞不再卡 VM，但连接池与超时纪律仍然适用于任何同步路径。
