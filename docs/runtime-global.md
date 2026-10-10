# 全局能力运行时语义

> 状态：P0 已落地（`SHIELD_ENABLE_GLOBAL=ON`；测试矩阵 `tests/lua_api/test_lua_api_global.cpp`）。
>
> 本文仍是 `shield_global` 的边界契约；P0 已实现：`shield.global()`（KV + 本地缓存）、互斥/读写/自旋/分布式锁门面、`shield.rank()` 排行榜、普通/延迟/优先级/广播/可靠队列、`shield.scheduler()`（cron/interval/once 与 pause/resume/remove/trigger）、`shield.rate_limiter()`（`allow`/`remaining`/有界 `wait`；token_bucket 与 sliding_window 双算法）。P0 的"分布式"锁、可靠队列与限流器共享进程内 `GlobalManager` 后端（token_bucket 的 Redis 周期同步、sliding_window 的 Redis 计数均为 Phase 2+ 形态；P0 两种算法全程进程内计算，零 Redis 调用，不存在"Redis 不可用"降级语义）；`shield.priority_queue` 与 `shield.broadcast_queue` 已实现（前者进程内多级队列，值越小越优先、同优先级 FIFO；后者进程内为有界 history + 每组 cursor，实时回调分发发生在推送方 VM，离线组在重新 subscribe 时按序补发——跨进程实时广播 Pub/Sub 留 Phase 2+）；KV 数据域的 Redis 后端已实现（`global.data_backend = redis`，见"配置"一节；锁/队列/排行/调度/限流的 Redis 后端仍为 Phase 2+，见文末范围表）。若与 [Lua API 契约](lua-api.md) 或 [配置语义](runtime-config.md) 冲突，以那两份文档为当前主线。

本文档包含 Shield 跨进程共享数据、分布式锁、排行榜、消息队列等全局能力的运行时语义决策。

`shield_global` 是官方可选模块，不属于 `shield_core`，也不是最小运行路径。最小部署路径通过数据插件 namespace 直接使用后端能力；本模块在显式配置的数据插件 binding 之上封装常见游戏全局能力。optional module 的横向 owner、配置归属和 disabled 语义见 [官方可选模块契约](optional-modules.md)。

## 设计原则

- 深度集成 Redis，封装常用游戏模式。（设计目标；P0 后端为进程内存，Redis 后端为 Phase 2+）
- 命名空间分离，职责清晰。
- 提供本地缓存，减少 Redis 压力。
- 统一接口，底层可切换。

## 命名空间

```lua
shield.global()               -- 全局数据（KV、缓存）
shield.mutex()                -- 本地互斥锁
shield.rwlock()               -- 本地读写锁
shield.spinlock()             -- 本地自旋锁
shield.distributed_mutex()    -- 分布式互斥锁
shield.distributed_rwlock()   -- 分布式读写锁
shield.rank()                 -- 排行榜
shield.queue()                -- 消息队列
shield.rate_limiter()         -- 限流器
shield.scheduler()            -- 定时任务调度
```

### 锁类型总览

| 创建方式 | 作用域 | 类型 | 存储 | 延迟 |
|----------|--------|------|------|------|
| `shield.mutex()` | 本地 | 互斥锁 | 内存 | 纳秒 |
| `shield.rwlock()` | 本地 | 读写锁 | 内存 | 纳秒 |
| `shield.spinlock()` | 本地 | 自旋锁 | 内存 | 纳秒 |
| `shield.distributed_mutex()` | 分布式 | 互斥锁 | 进程内存（P0；Redis 后端留 Phase 2+） | 纳秒级（P0 与本地锁同层） |
| `shield.distributed_rwlock()` | 分布式 | 读写锁 | 进程内存（P0；Redis 后端留 Phase 2+） | 纳秒级（P0 与本地锁同层） |

> P0 实现状态：`shield.distributed_mutex()` / `shield.distributed_rwlock()` 直接走本地锁工厂，与 `shield.mutex()` / `shield.rwlock()` 共享同一进程内 registry——"跨进程"由部署形态决定，后端本身不做跨进程协调。

### 通用锁接口

所有锁类型共享统一接口：

```lua
local l = shield.mutex("my_lock", {
    ttl = 30000,              -- 锁超时（ms）；0/缺省 = 永不过期
    retry = 100,              -- 轮询间隔（ms）；缺省 20
})
-- opts 只读取 ttl 与 retry；无 max_retries / reentrant 选项（重入恒开，按 owner token 计数）

-- 单次尝试（非阻塞；无参 acquire 不轮询）
l:acquire()

-- 时限内按 retry 间隔轮询
l:acquire(5000)

-- 非阻塞获取（与无参 acquire 等价）
local ok = l:try_acquire()

-- 释放锁
l:release()

-- 自动获取释放
l:with(function()
    -- 临界区代码
end)

-- 续期
l:extend(30000)
```

> 未实现：互斥/自旋/分布式互斥锁对象只有 `try_acquire` / `acquire` / `release` / `extend` / `owner` / `ttl` / `with` 七个方法（读写锁 guard 为 `try_acquire` / `acquire` / `release` / `with`）；不存在 `auto_extend`，也没有任何后台自动续期。

### 互斥锁

```lua
-- 本地互斥锁
local l = shield.mutex("my_lock", { ttl = 30000 })

l:acquire()
-- 临界区
l:release()

-- 或使用 with 自动管理
l:with(function()
    -- 临界区
end)
```

### 读写锁

```lua
-- 本地读写锁
local l = shield.rwlock("my_lock")

-- 读锁（共享，可并发）
local rl = l:read_lock()
rl:acquire()
-- 读操作...
rl:release()

-- 写锁（独占）
local wl = l:write_lock()
wl:acquire()
-- 写操作...
wl:release()

-- 自动管理
l:read_lock():with(function()
    -- 读操作
end)

l:write_lock():with(function()
    -- 写操作
end)
```

### 自旋锁

```lua
-- 自旋锁（忙等待，适合短临界区）
local l = shield.spinlock("my_lock")

l:acquire()
-- 极短的临界区
l:release()
```

### 分布式锁

```lua
-- 分布式互斥锁
local l = shield.distributed_mutex("my_lock", {
    ttl = 30000,
    retry = 100,
})

l:acquire()
-- 临界区（P0 与本地锁同为进程内后端）
l:release()

-- 分布式读写锁
local l = shield.distributed_rwlock("my_lock")

l:read_lock():with(function()
    -- 读操作
end)

l:write_lock():with(function()
    -- 写操作
end)
```

## 架构

```
┌─────────────────────────────────────────────────────────┐
│                  Lua 业务代码                            │
│  shield.global / lock / rank / queue / rate_limiter     │
├─────────────────────────────────────────────────────────┤
│                  shield_global                           │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐   │
│  │  Data    │ │   Lock   │ │   Rank   │ │  Queue   │   │
│  │ 全局数据  │ │ 分布式锁  │ │ 排行榜   │ │ 消息队列  │   │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘   │
│  ┌──────────┐ ┌──────────┐                              │
│  │  Rate    │ │   Pub    │                              │
│  │ 限流器   │ │   Sub    │                              │
│  └──────────┘ └──────────┘                              │
├─────────────────────────────────────────────────────────┤
│          数据插件 binding（cache/queue/leaderboard）      │
└─────────────────────────────────────────────────────────┘
```

---

## 一、全局数据

```lua
local g = shield.global()

-- 基础 KV
g:set("key", value, ttl)
local value = g:get("key")
g:delete("key")

-- 批量操作
g:mset({ key1 = "v1", key2 = "v2" })
local values = g:mget("key1", "key2")

-- 原子操作
g:incr("counter", 1)
g:decr("counter", 1)

-- 本地缓存（高频读取）
local value = g:get_cached("key", 60000)  -- 60秒缓存
g:invalidate("key")
```

### 配置

P0 只消费 `global.cache.max_size` 与 `global.cache.default_ttl` 两键；`global.cache.enabled` 与 `global.cache.sync`（跨进程缓存同步）不被解析。P0 全局仅三键生效（第三个是 `global.scheduler.tick_ms`，见文末"配置示例"总说明）。

```yaml
global:
  cache:
    max_size: 10000
    default_ttl: 60000
```

#### 数据域 Redis 后端（已实现）

KV 数据域支持把存储切到 Redis：`global.data_backend` 取 `""`（默认，进程内存）或 `"redis"`；选 `redis` 时 `global.redis.host` 必填，其余键可省：

```yaml
global:
  data_backend: redis
  redis:
    host: 127.0.0.1
    port: 6379          # 默认 6379
    password: ""        # 默认空（不鉴权）
    db: 0               # 默认 0
    prefix: shield:global  # 键前缀，实际存储键为 `<prefix>:<key>`
```

实现语义（`GlobalDataBackend` 接缝，`src/global/global_data_backend.cpp`）：

- **懒连接**：构造永不阻塞；首次命令才建立 redis++ 连接。连接失败时命令抛异常，向上传到 Lua 面（无静默降级——跨进程后端静默回退进程内存会造成脑裂）。
- **TTL**：`set(key, value, ttl)` 的 `ttl` 经 PEXPIRE 下发（毫秒精度）；`ttl = 0` 表示永不过期，且会 PERSIST 清掉上次写入留下的 TTL（与进程内语义一致）。
- **incr**：INCRBY；对非整数值 Redis 报错，映射为与进程内相同的错误串（`value of '...' is not an integer`）。
- **mget/mset**：MGET / MSET（mset 再逐键 PEXPIRE）；`size()` 用 SCAN `prefix:*` 计数。
- **缓存一致性**：本地缓存（LRU）留在 `GlobalManager` 委托层——写、删、incr 与惰性过期都会丢弃该键的缓存副本。本地缓存是按 TTL 有界的：Redis 后端下其他进程写入本进程不会立刻感知，缓存副本在自身 TTL 到期前可能短暂陈旧。
- **隔离**：不同部署共用一个 Redis 实例时用不同 `prefix`。

---

## 二、分布式锁

> P0 实现状态：不存在 `shield.lock()` 工厂（`shield` 表只注册了 mutex/spinlock/rwlock/distributed_mutex/distributed_rwlock）。锁在工厂创建时绑定名字，获取/释放在锁对象上进行；P0 后端为进程内（见锁类型总览表下标注）。以下为当前可用的两步写法。

```lua
-- 第一步：工厂创建锁对象（名字在此绑定）
local l = shield.distributed_mutex("my_lock", {
    ttl = 30000,              -- 锁超时；0/缺省 = 永不过期
    retry = 100,              -- 轮询间隔（ms）
})

-- 第二步：在锁对象上获取
local ok = l:acquire(5000)    -- 时限内按 retry 间隔轮询；无参 = 单次尝试
if ok then
    l:extend(30000)           -- 续期
    l:owner()                 -- 锁信息（见"锁信息"）
    l:ttl()                   -- 剩余时间（ms）；无锁返回 -1
    l:release()               -- 释放
end

-- 非阻塞
local ok = l:try_acquire()
```

### 锁信息

`l:owner()` / `lock_info` 返回的实际形状（`owner` 是门面铸造的单字符串不透明 token，不是结构化表）：

```lua
{
    exists = true,                -- 锁是否存在（不存在时整个返回 nil）
    owner = "mutex:my_lock:table: 0x…",  -- 不透明 owner token
    count = 1,                    -- 重入持有计数
    acquired_at = 1234567890000,  -- 获取时间（ms）
    ttl_remaining = 30000,        -- 剩余 TTL（ms）；0 = 无 TTL / 无锁
}
```

无 `key`、`node_id`/`service_id` 等分解字段，也无 `reentrant_count`（重入计数字段名为 `count`）。

### 实现机制

> P0 实现状态：锁后端为进程内 C++ 容器 + 互斥（`GlobalManager` 的 mutexes_/rwlocks_/spinlocks_ 表加 per-domain mutex），不使用 Redis，也没有任何 Lua 脚本。以下 Redis Lua 脚本与 Redis 数据结构均为 Phase 2+（Redis 后端）的设计意图，当前未实现。

**设计意图（Phase 2+）：** 所有锁操作使用 Lua 脚本保证原子性，避免竞态条件。

**Redis 数据结构：**

```
Hash: shield:lock:{name}
  field: owner   → "node_id:service_id:coroutine_id"
  field: count   → 重入计数（整数）

Sorted Set: shield:lock:queue:{name}  （仅公平锁）
  member → owner_id
  score  → 等待时间戳
```

**获取锁（Lua 脚本，原子）：**

```lua
-- KEYS[1] = shield:lock:{name}
-- ARGV[1] = owner_id, ARGV[2] = ttl_ms
local owner = redis.call('HGET', KEYS[1], 'owner')
if owner == false then
    -- 无主，直接获取
    redis.call('HSET', KEYS[1], 'owner', ARGV[1], 'count', 1)
    redis.call('PEXPIRE', KEYS[1], ARGV[2])
    return 1
elseif owner == ARGV[1] then
    -- 同一 owner，可重入，count +1
    redis.call('HINCRBY', KEYS[1], 'count', 1)
    redis.call('PEXPIRE', KEYS[1], ARGV[2])
    return 1
else
    -- 其他 owner 持有，获取失败
    return 0
end
```

**释放锁（Lua 脚本，原子）：**

```lua
-- KEYS[1] = shield:lock:{name}
-- ARGV[1] = owner_id
local owner = redis.call('HGET', KEYS[1], 'owner')
if owner ~= ARGV[1] then
    -- 非持有者，释放失败
    return 0
end
local count = redis.call('HINCRBY', KEYS[1], 'count', -1)
if count <= 0 then
    -- 重入归零，删除锁
    redis.call('DEL', KEYS[1])
    return 1
else
    return count
end
```

**续期锁（Lua 脚本，原子）：**

```lua
-- KEYS[1] = shield:lock:{name}
-- ARGV[1] = owner_id, ARGV[2] = ttl_ms
local owner = redis.call('HGET', KEYS[1], 'owner')
if owner ~= ARGV[1] then
    return 0
end
redis.call('PEXPIRE', KEYS[1], ARGV[2])
return 1
```

**TTL 与过期（P0 实际行为）：**

锁默认无 TTL（`ttl = 0` = 永不过期），TTL 由创建时的 `opts.ttl` 指定。TTL 到期为**惰性接管**：没有后台清理线程，下一个获取者发现锁已过期即直接接管。进程崩溃后持锁永不释放（进程内状态随进程消亡），P0 也没有退出路径上的主动释放逻辑。跨进程"崩溃释放"依赖 Phase 2+ 的 Redis 后端。

**自动续期（未实现）：**

不存在 `auto_extend` 方法与后台续期协程；续期只能显式调用 `l:extend(ttl)`。后台续期属 Phase 2+ 设计（上文 Redis Lua 脚本形态）。

**公平锁：**

使用 Sorted Set 做等待队列，acquire 时入队，按 score（时间戳）顺序获取锁。release 时从队列移除，唤醒下一个等待者。队列操作也使用 Lua 脚本保证原子性。

### 配置

> 未接线：`lock:` 配置节在 P0 完全不被解析（锁参数只来自 Lua 工厂 opts）。P0 全局仅三键生效（`global.cache.max_size` / `global.cache.default_ttl` / `global.scheduler.tick_ms`，见文末"配置示例"总说明）。以下为 Phase 2+ 设计意图。

```yaml
lock:
  enabled: true
  default_ttl: 30000
  max_ttl: 300000
  reentrant: true
  auto_extend:
    enabled: true
    interval: 10000
  fair:
    enabled: false            # 公平锁
  redlock:
    enabled: false            # 多节点强一致锁
    quorum: 3
```

---

## 三、排行榜

```lua
local rank = shield.rank("player_score")

-- 更新分数
rank:update("player_1", 1000)
rank:mupdate({ player_1 = 1000, player_2 = 950 })

-- 查询排名
local pos = rank:position("player_1")     -- 排名（从1开始）
local score = rank:score("player_1")       -- 分数

-- Top N
local top10 = rank:top(10)

-- 范围查询
local range = rank:range(1, 100)           -- 按排名
local by_score = rank:range_by_score(900, 1100)  -- 按分数

-- 周围排名
local around = rank:around("player_1", 5)  -- 前后各5名

-- 其他
local total = rank:count()
rank:remove("player_1")
rank:clear()
```

### 返回格式

```lua
-- top(10)
{
    { uid = "player_3", score = 1100, rank = 1 },
    { uid = "player_1", score = 1000, rank = 2 },
    -- ...
}

-- around("player_1", 5)
{
    above = { ... },
    target = { uid = "player_1", score = 1000, rank = 2 },
    below = { ... },
}
```

### 定时重置

> 未实现：P0 的 `shield.rank(name)` 只接收榜单名一个参数（lua_api.cpp），不存在 `reset` / `reset_time` / `archive` / `distributed` 声明式选项，也不会自动注册 scheduler 任务。以下为 Phase 2+ 设计意图；当前需业务自行注册 cron 任务并调用 `rank:clear()`（注意 `scheduler:cron` 的第 4 参 opts 当前被整体忽略，`{ distributed = true }` 不生效）。

设计意图——声明式配置，内部自动注册 scheduler 任务：

```lua
local daily_rank = shield.rank("daily_score", {
    reset = "daily",          -- daily | weekly | monthly
    reset_time = "00:00",     -- 重置时间（UTC）
    archive = true,           -- 归档旧数据
    distributed = true,       -- 分布式调度（只在一个进程执行）
})
```

等价于手动 scheduler：

```lua
local scheduler = shield.scheduler()
local rank = shield.rank("daily_score")

scheduler:cron("rank:daily_score:reset", "0 0 * * *", function()
    if rank_config.archive then
        -- 归档到 Redis Hash: shield:rank:archive:daily_score:{date}
        local data = rank:top(rank_config.max_members or 10000)
        local date = os.date("%Y%m%d")
        redis.hset("shield:rank:archive:daily_score:" .. date, "data", json.encode(data))
        redis.expire("shield:rank:archive:daily_score:" .. date, 2592000) -- 30天
    end
    rank:clear()
end, { distributed = true })
```

**归档存储：**

```
Redis Hash: shield:rank:archive:{rank_name}:{YYYYMMDD}
  field: "data" → JSON 序列化的 top N 数据
  TTL: 30 天（可配置）
```

**重置触发方式：**

| 方式 | 说明 | 适用场景 |
|------|------|----------|
| 声明式 `reset` 配置 | 自动注册 scheduler 任务 | 标准周期重置 |
| 手动 scheduler | 业务自行注册 cron 任务 | 自定义归档逻辑 |
| 手动 `rank:clear()` | 立即清空 | 运维手动重置 |

### 配置

> 未接线：`rank:` 配置节在 P0 完全不被解析（榜单无 max_members/归档配置可调）。P0 全局仅三键生效（见文末"配置示例"总说明）。以下为 Phase 2+ 设计意图。

```yaml
rank:
  enabled: true
  max_members: 10000
  reset:
    enabled: true
    archive_table: "rank_archive"
    archive_ttl: 2592000       # 归档保留天数（秒），默认 30 天
```

---

## 四、消息队列

### 队列类型

| 类型 | 创建方式 | 适用场景 |
|------|----------|----------|
| 普通队列 | `shield.queue("name")` | 异步任务处理 |
| 延迟队列 | `shield.delay_queue("name")` | 延迟奖励、定时任务 |
| 优先级队列 | `shield.priority_queue("name")` | 紧急任务优先 |
| 可靠队列 | `shield.reliable_queue("name")` | 关键业务，消费确认 |
| 广播队列 | `shield.broadcast_queue("name")` | 事件通知，多消费者 |

### 普通队列

```lua
local q = shield.queue("task_queue")

-- 生产
q:push("task_data")
q:push({ type = "email", to = "user@example.com" })
q:push_batch({"task1", "task2"})

-- 消费
local msg = q:pop()           -- 非阻塞
local msg = q:pop(5000)       -- 阻塞等待5秒

-- 其他
local len = q:length()
q:purge()
```

### 延迟队列

```lua
local q = shield.delay_queue("delay_task")

-- 延迟投递
q:push("reward_data", 60000)              -- 延迟60秒
q:push_at("reward_data", os.time() + 3600) -- 指定时间

-- 消费到期消息
local msg = q:pop(5000)

-- 查看状态
local pending = q:pending()    -- 未到期
local ready = q:ready()        -- 已到期
```

### 优先级队列

```lua
local q = shield.priority_queue("priority_task")

q:push("urgent_task", 1)      -- 最高优先级
q:push("normal_task", 5)
q:push("low_task", 10)

local msg = q:pop()           -- 返回 urgent_task
```

### 可靠队列

```lua
local q = shield.reliable_queue("reliable_task")

q:push("important_task")

-- 消费（带确认）
local msg, handle = q:pop(5000)
if msg then
    local ok = pcall(process, msg)
    if ok then
        handle:ack()            -- 确认成功
    else
        handle:nack(30000)      -- 失败，30秒后重试
    end
end

-- 死信队列（重试耗尽）
local dead = q:dead_letter()
local dead_msgs = dead:range(0, 100)
```

### 广播队列

```lua
local q = shield.broadcast_queue("events")

-- 订阅（消费者组）
q:subscribe("ui_service", function(msg)
    show_notification(msg)
end)

q:subscribe("achievement_service", function(msg)
    check_achievement(msg)
end)

-- 生产（所有消费者组收到）
q:push({ type = "boss_killed", uid = "player_1" })
```

P0 进程内口径：`q` 另提供 `unsubscribe(group)`（仅解除本地回调，组 cursor 保留用于离线补发）、`history()`（保留条数）、`groups()`（已跟踪组数）、`purge()`；`max_history` 是 `shield.broadcast_queue(name, { max_history = N })` 工厂 opts（`q:subscribe(group, fn)` 不接收 opts；缺省 1000，工厂传入时即改写存量上限）。回调分发发生在推送方 VM（C++ 后端不持有 Lua 引用）；离线/其他 VM 的组通过 history + cursor 在重新 `subscribe` 时补发；跨进程实时广播（Pub/Sub + Stream）留 Phase 2+。

### 实现机制

> P0 实现状态：所有队列均为进程内 C++ 容器 + 互斥（`GlobalManager`），不使用 Redis，也没有 Redis Lua 脚本。以下 Redis 数据结构（含 Stream 消费者组、XCLAIM 协调）为 Phase 2+（Redis 后端）的设计意图，当前未实现。

**设计意图（Phase 2+）：** 各队列类型使用不同的 Redis 数据结构：

| 队列类型 | Redis 结构 | 说明 |
|----------|-----------|------|
| 普通队列 | List | `LPUSH` 生产，`RPOP`/`BRPOP` 消费 |
| 延迟队列 | Sorted Set | score 为到期时间戳，`ZRANGEBYSCORE` 取到期消息 |
| 优先级队列 | Sorted Set | score 为优先级值（越小越优先），`ZPOPMIN` 消费 |
| 可靠队列 | Stream | 使用 `XADD`/`XREADGROUP`/`XACK`，原生消费者组 |
| 广播队列 | Pub/Sub + Stream | Pub/Sub 实时广播，Stream 做离线消息补发 |

**可靠队列 ACK/NACK 机制：**

基于 Redis Stream 的消费者组（Consumer Group）：

```
生产: XADD shield:queue:reliable_task * data "{...}"
消费: XREADGROUP GROUP worker consumer-1 COUNT 1 BLOCK 5000 STREAMS shield:queue:reliable_task >
确认: XACK shield:queue:reliable_task worker <message-id>
```

NACK（失败重试）将消息重新分配：

```
NACK: XCLAIM shield:queue:reliable_task worker consumer-1 <retry_after_ms> <message-id>
```

死信转储时机（P0 与 Phase 2+ 同语义）：**重试次数达到 `max_retries` 即入死信**（`retries >= max_retries`，默认 3——即第 3 次 nack 就转死信，不是"超过 3 次"）。P0 的 `max_retries` 来自 `shield.reliable_queue(name, { max_retries = N })` 工厂 opts，进程内容器执行（死信为内存列表）；"独立 Stream" 为 Phase 2+ 形态。

**消费者组协调（Phase 2+ 设计意图，P0 无 Redis Stream）：**

- 每个进程启动时注册为独立 consumer
- Stream 的消费者组保证每条消息只被一个 consumer 处理
- 进程退出后，其 pending 消息超过 idle 时间后可被其他 consumer 认领（`XCLAIM`）

### 配置

> 未接线：`queue:` 配置节在 P0 完全不被解析（`max_retries` 等只来自 `shield.reliable_queue` 工厂 opts，缺省 3）。P0 全局仅三键生效（见文末"配置示例"总说明）。以下为 Phase 2+ 设计意图。

```yaml
queue:
  enabled: true
  defaults:
    max_length: 100000
    message_ttl: 86400000       # 24小时
    max_retries: 3
    retry_delay: 60000
  dead_letter:
    enabled: true
    max_length: 10000
    ttl: 604800000              # 7天
```

---

## 五、限流器

业务级限流。与网关层连接级限流（`rate_limit`，见 [安全语义](runtime-security.md#速率限制网关层)）不同：

> P0 实现状态：限流器完全进程内（token bucket / sliding window 均为 `GlobalManager` 内存结构，零 Redis 调用）。"全服统一配额"（跨进程汇总）依赖 Redis 周期同步，属 Phase 2+；P0 的配额是**本进程**配额。

| 层级 | 作用 | 存储 | 适用场景 |
|------|------|------|----------|
| 网关层 `rate_limit` | 按客户端 IP/连接限流 | 内存 | 防刷、防 DDoS |
| 业务层 `shield.rate_limiter()` | 按业务 key 限流 | 进程内存（P0；本地+Redis 为 Phase 2+） | 全服限流、API 限流 |

### 实现策略

**P0：** 纯进程内实现——`allow()` 内存判断，无任何 Redis 交互，也不存在同步线程；配额以本进程为单位。

**Phase 2+ 设计意图（未实现）：本地令牌桶 + Redis 周期同步。** 每次 `allow()` 不请求 Redis，使用本地令牌桶纳秒级判断。周期性与 Redis 同步全局配额。

```
请求到达
  │
  ├─ 本地令牌桶判断（纳秒级）
  │   ├─ 有令牌 → 放行，消耗令牌
  │   └─ 无令牌 → 拒绝
  │
  └─ 每 sync_interval 同步到 Redis
      ├─ 上报本进程消费量
      ├─ 拉取全局已消耗总量
      ├─ 按进程数分配本地配额
      └─ 调整本地桶容量
```

**配额分配：**

```
全局速率 = 100/s，3 个进程
  → 每进程分配 ~33/s
  → 本地令牌桶按 33/s 速率补充令牌
  → 周期同步时按实际消耗重新分配
```

**精度特性（量级参考，以压测为准；下两行为 Phase 2+ Redis 同步形态，P0 无同步开销）：**

| 指标 | 值 |
|------|------|
| 判断延迟 | < 1μs（本地内存，量级参考） |
| 同步延迟 | 1-5ms（Redis round-trip，Phase 2+ 才存在） |
| 精度误差 | < sync_interval（默认 1s，误差 < 1 秒内请求量） |
| 最终一致性 | 同步后全局配额精确 |

### Lua API

```lua
local limiter = shield.rate_limiter("api_limit", {
    rate = 100,                 -- 每秒 100 请求（P0 为本进程配额）
    burst = 200,                -- 突发容量 200
})

-- 检查（本地判断，不请求 Redis）
local allowed = limiter:allow("client_ip")
local remaining = limiter:remaining("client_ip")

-- 阻塞等待（本地等待，不请求 Redis）
limiter:wait("client_ip", 5000)
```

### 滑动窗口

P0 的滑动窗口为进程内 deque 精确计数（窗口内未超限即正常放行），每次 `allow()` 零 Redis 调用、不发 EVAL。适合低频精确限流场景；"每次 1 次 Redis 调用"是 Phase 2+（Redis 计数）形态：

```lua
local limiter = shield.rate_limiter("api_strict", {
    window = 60000,             -- 1 分钟窗口
    max_requests = 100,         -- 窗口内最多 100 次
    sliding = true,             -- 滑动窗口（P0 为进程内 deque）
})
```

### 算法对比

| 算法 | 每次请求 Redis（P0） | 精度 | 适用场景 |
|------|---------------|------|----------|
| `token_bucket` | 否 | 近似 | 高频业务限流（默认） |
| `sliding_window` | 否 | 精确 | 低频精确限流 |

（Phase 2+ 的 Redis 后端形态下 `sliding_window` 每次请求 1 次 Redis EVAL；P0 两算法均为纯本地、零 Redis 调用。）

### 配置

> 未接线：`rate_limiter:` 配置节在 P0 完全不被解析（algorithm / sync_interval / cleanup_interval 均无效果）；限流参数只来自 `shield.rate_limiter(name, opts)` 的 Lua opts（rate / burst / sliding / window / max_requests）。P0 全局仅三键生效（见文末"配置示例"总说明）。以下为 Phase 2+ 设计意图。

```yaml
rate_limiter:
  enabled: true
  algorithm: "token_bucket"    # token_bucket | sliding_window
  sync_interval: 1000          # 本地与 Redis 同步间隔（ms），仅 token_bucket
  cleanup_interval: 60000      # 过期 key 清理间隔（ms）
```

### 降级策略

**P0 无降级路径**：限流全程进程内，没有 Redis 依赖，不存在"Redis 不可用"场景；进程内滑动窗口计数始终可用（不存在"Redis 不可用全拒"行为）。下表仅适用于 Phase 2+ 的 Redis 同步形态（届时降级策略另行定义，"拒绝所有请求"一列不成立）。

| 场景 | token_bucket 行为 | sliding_window 行为（Phase 2+ 设计意图） |
|------|-------------------|---------------------|
| Redis 不可用 | 继续使用本地配额，日志告警 | 待定（依赖 Redis 计数，P0 进程内实现无此场景） |
| Redis 恢复 | 下次同步自动恢复全局配额 | 自动恢复 |
| 网络延迟高 | 同步延迟，本地配额可能偏大/偏小 | 请求延迟增加 |

token_bucket 模式下 Redis 故障不影响业务可用性，只影响全局配额精度。

---

## 六、Pub/Sub

> 未实现：P0 的 `shield.global()` 返回的 `g` 表只有 9 个方法（set / get / delete / incr / decr / mset / mget / get_cached / invalidate），不存在 `g:subscribe` / `g:psubscribe` / `g:publish`。跨进程 Pub/Sub 是 Phase 2+；进程内的组回调分发由 `shield.broadcast_queue` 提供（见"四、消息队列·广播队列"）。以下为 Phase 2+ 设计意图。

```lua
local g = shield.global()

-- 订阅
g:subscribe("system_notice", function(channel, message)
    shield.log.info("received: " .. message.text)
end)

-- 模式订阅
g:psubscribe("player.*", function(channel, message)
    -- 处理 player.1, player.2 等
end)

-- 发布
g:publish("system_notice", { text = "Hello!" })
```

---

## 配置示例

> **P0 配置生效总说明**：`shield_global` 在 P0 只消费 3 个配置键——`global.cache.max_size`、`global.cache.default_ttl`、`global.scheduler.tick_ms`（`GlobalConfig::from_global_config`）。其余 `global.*`、`lock.*`、`rank.*`、`queue.*`、`rate_limiter.*`、`scheduler.*` 等节均不被解析（各节内已标注"未接线"）；本文中未标注的 yaml 一律视为 Phase 2+ 设计意图。限流/锁/队列的运行参数只经 Lua API 传入（工厂 opts）。

```yaml
global:
  enabled: true                  # 未解析（模块由 SHIELD_ENABLE_GLOBAL 开关）
  redis:                         # 未解析（Redis 后端为 Phase 2+）
    inherit: true
    db: 1
    prefix: "shield:global:"

  cache:
    enabled: true                # 未解析
    max_size: 10000              # 生效
    default_ttl: 60000           # 生效
    sync: true                   # 未解析（跨进程缓存同步为 Phase 2+）

lock:
  enabled: true                  # 未解析
  default_ttl: 30000             # 未解析（锁 TTL 来自 Lua 工厂 opts）
  reentrant: true                # 未解析（重入恒开）

rank:
  enabled: true                  # 未解析
  max_members: 10000             # 未解析

queue:
  enabled: true                  # 未解析
  defaults:
    max_length: 100000           # 未解析
    message_ttl: 86400000        # 未解析

rate_limiter:
  enabled: true                  # 未解析
  algorithm: "token_bucket"      # 未解析（算法由 Lua opts 的 sliding 选择）
```

---

## 使用示例

### 全服BOSS

```lua
local l = shield.distributed_mutex("boss_spawn", { ttl = 60000 })

function M.spawn_boss()
    if not l:acquire() then return nil, "busy" end  -- 单次尝试，拿不到即忙

    local g = shield.global()
    local boss = g:get("world_boss")
    if boss and boss.status == "alive" then
        l:release()
        return nil, "boss_alive"
    end

    g:set("world_boss", { hp = 10000, status = "alive" })
    l:release()
    return true
end
```

### 全服排行榜

```lua
local rank = shield.rank("damage_rank")

function M.update_damage(uid, damage)
    local current = rank:score(uid) or 0
    if damage > current then
        rank:update(uid, damage)
    end
end

function M.get_top100()
    return rank:top(100)
end
```

### 延迟奖励

```lua
local q = shield.delay_queue("daily_reward")

function M.on_login(player)
    q:push({ uid = player.uid, reward = "daily_bonus" }, 86400000)
end

-- 消费：delay_queue 没有 consume/subscribe，轮询 pop 取到期消息
local function consume_loop()
    while true do
        local msg = q:pop(5000)   -- 最多等 5 秒；超时返回 nil
        if msg then
            give_reward(msg.uid, msg.reward)
        end
        -- 也可用 q:ready() 判断是否有到期消息
    end
end
```

### 事件广播

```lua
local q = shield.broadcast_queue("game_events")

q:subscribe("ui_service", function(msg)
    if msg.type == "boss_killed" then
        show_notification(msg)
    end
end)

function M.on_boss_killed(uid)
    q:push({ type = "boss_killed", uid = uid })
end
```

---

## ops 暴露

> 未实现：P0 的 ops HTTP 服务只注册 `/ops/health`、`/ops/status`、`/ops/metrics`、`/ops/services`(+`/:name`)、`/ops/plugins`、`/ops/config`（以及 opt-in 的 `/ops/eval`、`/ops/profile`）；不存在 `GET /ops/global`、`GET /ops/scheduler` 路由。统计所需的 C++ 接口在 `GlobalManager` 上已有，缺的是 HTTP 暴露。以下 JSON 为设计意图。

```json
GET /ops/global

{
  "data": {
    "cache_size": 1500,
    "cache_hit_rate": 0.95
  },
  "locks": {
    "active": 5,
    "waiting": 2
  },
  "ranks": {
    "count": 3,
    "total_members": 5000
  },
  "queues": [
    {
      "name": "task_queue",
      "length": 150,
      "consumers": 2
    },
    {
      "name": "delay_task",
      "pending": 500,
      "ready": 50
    }
  ],
  "rate_limiter": {
    "active": 10,
    "rejected_today": 500
  }
}
```

---

## 七、定时任务调度

支持 cron 表达式的定时任务调度，用于每日重置、定时活动等。

### 基础用法

```lua
local scheduler = shield.scheduler()

-- 添加定时任务
scheduler:cron("daily_reset", "0 0 * * *", function()
    -- 每天 00:00 执行
    reset_daily_data()
end)

-- 添加间隔任务
scheduler:interval("heartbeat", 60000, function()
    -- 每 60 秒执行
    send_heartbeat()
end)

-- 添加一次性任务
scheduler:once("delayed_task", 3600000, function()
    -- 1 小时后执行
    send_reward()
end)
```

### Cron 表达式

```lua
-- 标准 cron 格式：分 时 日 月 周
scheduler:cron("task", "0 0 * * *", fn)      -- 每天 00:00
scheduler:cron("task", "*/5 * * * *", fn)    -- 每 5 分钟
scheduler:cron("task", "0 * * * *", fn)      -- 每小时整点
scheduler:cron("task", "0 0 * * 1", fn)      -- 每周一 00:00
scheduler:cron("task", "0 0 1 * *", fn)      -- 每月 1 号 00:00
scheduler:cron("task", "0 12 * * 1-5", fn)   -- 工作日 12:00
```

cron 为标准 5 字段（分 时 日 月 周），分钟粒度，按 **UTC** 解释；不支持时区参数——`scheduler:cron(name, expr, fn, opts)` 的第 4 参 opts 在 P0 被整体忽略（传 `timezone` 不会有任何效果，也不报错）。

### 任务管理

```lua
-- 获取任务
local task = scheduler:get("daily_reset")

-- 任务信息：get 返回纯字段表（不是可调用对象，没有 task:name() 等方法）
task.name                     -- 任务名
task.type                     -- "cron" | "interval" | "once"
task.schedule                 -- cron 表达式或间隔 ms
task.next_run                 -- 下次执行时间（ms）
task.last_run                 -- 上次执行时间（ms）
task.run_count                -- 执行次数
task.status                   -- "active" | "paused" | "done"（无 "error" 状态）

-- 暂停/恢复
scheduler:pause("daily_reset")
scheduler:resume("daily_reset")

-- 删除任务
scheduler:remove("daily_reset")

-- 手动触发
scheduler:trigger("daily_reset")
```

### 分布式调度

> 未实现：`scheduler:cron/interval/once` 的第 4 参 opts（含 `distributed` / `lock_ttl`）在 P0 被整体丢弃——任务在每个加载它的进程内各自执行，没有分布式互斥。跨进程互斥目前需业务在回调内自行用 `shield.distributed_mutex` 包裹。以下为 Phase 2+ 设计意图。

多进程部署时，确保同一任务只在一个进程执行：

```lua
-- 分布式任务（自动分布式锁）
scheduler:cron("daily_reset", "0 0 * * *", function()
    reset_daily_data()
end, {
    distributed = true,       -- 启用分布式调度
    lock_ttl = 30000,         -- 锁超时
})
```

### 任务失败处理

> 未实现：任务 opts 在 P0 被整体丢弃，`max_retries` / `retry_delay` / `on_error` 均无效果；失败处理的实际行为只是回调 pcall 后 `log.warn("scheduler task failed: …")`，无重试、无错误回调。以下为 Phase 2+ 设计意图。

```lua
scheduler:cron("risky_task", "0 * * * *", function()
    risky_operation()
end, {
    max_retries = 3,          -- 最大重试次数
    retry_delay = 60000,      -- 重试延迟
    on_error = function(err)
        shield.log.error("task failed: " .. err)
    end,
})
```

### 配置

> 未接线：`scheduler:` 配置节在 P0 完全不被解析（分布式锁、失败重试、redis 持久化均无对应开关）。P0 与调度器相关的唯一生效键是 `global.scheduler.tick_ms`（默认 250，调度器轮询间隔），见文末"配置示例"总说明。以下为 Phase 2+ 设计意图。

```yaml
scheduler:
  enabled: true

  # 分布式调度
  distributed:
    enabled: true
    lock_ttl: 30000

  # 任务失败重试
  retry:
    max_retries: 3
    retry_delay: 60000

  # 任务持久化
  persistence:
    enabled: true
    storage: redis
```

### 使用示例

**每日重置排行榜：**

```lua
local scheduler = shield.scheduler()
local rank = shield.rank("daily_score")

scheduler:cron("daily_rank_reset", "0 0 * * *", function()
    -- 归档昨日数据
    local yesterday = rank:top(100)
    archive_rank("daily_score", yesterday)

    -- 清空排行榜
    rank:clear()

    shield.log.info("daily rank reset completed")
end)   -- 不传第 4 参：opts 在 P0 被忽略（见"分布式调度"标注）
```

**定时活动：**

```lua
local scheduler = shield.scheduler()
local g = shield.global()
local events = shield.broadcast_queue("activity_events")  -- P0 无 g:publish

-- 每周六 20:00 开启双倍经验活动
scheduler:cron("double_exp_event", "0 20 * * 6", function()
    g:set("activity.double_exp", {
        status = "active",
        start_time = os.time(),
        end_time = os.time() + 7200,  -- 2小时
        multiplier = 2,
    })
    events:push({ type = "activity_started", name = "double_exp" })
end)

-- 每周六 22:00 关闭活动
scheduler:cron("double_exp_end", "0 22 * * 6", function()
    g:set("activity.double_exp", { status = "inactive" })
    events:push({ type = "activity_ended", name = "double_exp" })
end)
```

**数据备份：**

```lua
local scheduler = shield.scheduler()

-- 每天凌晨 3 点备份数据（UTC；cron 不带 opts）
scheduler:cron("data_backup", "0 3 * * *", function()
    backup_player_data()
    backup_global_data()
    shield.log.info("data backup completed")
end)
```

### ops 暴露

> 未实现：不存在 `GET /ops/scheduler` 路由（ops HTTP 只注册 health/status/metrics/services/plugins/config 与 opt-in 的 eval/profile）。调度器统计可从 `GlobalManager` 取得，缺 HTTP 暴露。以下 JSON 为设计意图。

```json
GET /ops/scheduler

{
  "tasks": [
    {
      "name": "daily_reset",
      "schedule": "0 0 * * *",
      "status": "active",
      "next_run": "2026-06-11T00:00:00Z",
      "last_run": "2026-06-10T00:00:00Z",
      "run_count": 30,
      "distributed": true
    },
    {
      "name": "heartbeat",
      "schedule": "60000",
      "status": "active",
      "next_run": "2026-06-10T12:01:00Z",
      "last_run": "2026-06-10T12:00:00Z",
      "run_count": 1440,
      "distributed": false
    }
  ]
}
```

## 实现优先级

| 功能 | 优先级 | 说明 |
|------|--------|------|
| 全局数据 + 本地缓存 | P0 | 核心功能；KV 数据域 Redis 后端已实现（`global.data_backend = redis`），本地缓存按 TTL 有界（跨进程写入存在短暂陈旧窗口） |
| 分布式锁（重入、续期） | P0 | 全局操作必需 |
| 排行榜 | P0 | 游戏标配 |
| 普通队列 | P0 | 异步任务 |
| 延迟队列 | P0 | 延迟奖励、定时任务 |
| 可靠队列 | P0 | 关键业务 |
| 定时任务调度 | P0 | 每日重置、定时活动 |
| 限流器 | P0（进程内；Redis 周期同步留 Phase 2+） | 防刷、防滥用 |
| Pub/Sub | P1 | 跨进程通知 |
| 优先级队列 | P0（进程内；Redis ZSET 后端留 Phase 2+） | 紧急任务 |
| 广播队列 | P0（进程内；跨进程 Pub/Sub 留 Phase 2+） | 事件通知 |
| Redlock | P2 | 强一致场景 |
