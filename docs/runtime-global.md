# 全局能力运行时语义

> 状态：设计草案，非当前实现契约。
>
> 本文冻结 `shield_global` optional module 的边界契约；下文给出的 `shield.global()`、锁、排行榜、队列、限流器等 Lua API **均未实现**，进入 Phase 2+。若与 [Lua API 契约](lua-api.md) 或 [配置语义](runtime-config.md) 冲突，以那两份文档为当前主线。

本文档包含 Shield 跨进程共享数据、分布式锁、排行榜、消息队列等全局能力的运行时语义决策。

`shield_global` 是官方可选模块，不属于 `shield_core`，也不是最小运行路径。最小部署路径只要求 `shield_data` 提供原始 DB / Redis 能力；本模块在此基础上封装常见游戏全局能力。optional module 的横向 owner、配置归属和 disabled 语义见 [官方可选模块契约](optional-modules.md)。

## 设计原则

- 深度集成 Redis，封装常用游戏模式。
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
| `shield.distributed_mutex()` | 分布式 | 互斥锁 | Redis | 毫秒 |
| `shield.distributed_rwlock()` | 分布式 | 读写锁 | Redis | 毫秒 |

### 通用锁接口

所有锁类型共享统一接口：

```lua
local l = shield.mutex("my_lock", {
    ttl = 30000,              -- 锁超时（ms）
    retry = 100,              -- 重试间隔（ms）
    max_retries = 0,          -- 最大重试次数，0=无限
    reentrant = true,         -- 可重入
})

-- 获取锁（阻塞，自动重试）
l:acquire()

-- 获取锁（带超时）
l:acquire(5000)

-- 非阻塞获取
local ok = l:try_acquire()

-- 释放锁
l:release()

-- 自动获取释放
l:with(function()
    -- 临界区代码
end)

-- 续期（分布式锁）
l:extend(30000)

-- 自动续期
l:auto_extend(true, 10000)
```

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
    reentrant = true,
})

l:acquire()
-- 跨进程临界区
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
│                  shield_data (Redis)                     │
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

```yaml
global:
  cache:
    enabled: true
    max_size: 10000
    default_ttl: 60000
    sync: true                # 跨进程缓存同步
```

---

## 二、分布式锁

```lua
local lock = shield.lock()

-- 获取锁（阻塞等待）
local l = lock:acquire("my_lock", {
    ttl = 30000,              -- 锁超时
    wait = 10000,             -- 最大等待
    retry = 100,              -- 重试间隔
    reentrant = true,         -- 可重入
})

-- 获取锁（非阻塞）
local l = lock:try_acquire("my_lock", { ttl = 30000 })

if l then
    l:extend(30000)           -- 续期
    l:owner()                 -- 持有者信息
    l:ttl()                   -- 剩余时间
    l:release()               -- 释放
end

-- 自动续期
l:auto_extend(true, 10000)   -- 每10秒续期
l:auto_extend(false)         -- 关闭自动续期
```

### 锁信息

```lua
{
    key = "my_lock",
    owner = {
        node_id = "node-1",
        service_id = 12345,
        service_name = "boss_manager",
        thread_id = "coroutine-67890",
    },
    acquired_at = 1234567890000,
    ttl = 30000,
    reentrant_count = 1,
}
```

### 实现机制

所有锁操作使用 Lua 脚本保证原子性，避免竞态条件。

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

**进程崩溃释放：**

锁带 TTL（默认 30s），进程崩溃后锁自动过期释放。正常退出时主动释放（不等 TTL）。

**自动续期：**

`auto_extend(true, interval)` 启动后台协程，每 interval 毫秒执行续期 Lua 脚本。续期前已通过 Lua 脚本验证 owner，不会续期已释放的锁。

**公平锁：**

使用 Sorted Set 做等待队列，acquire 时入队，按 score（时间戳）顺序获取锁。release 时从队列移除，唤醒下一个等待者。队列操作也使用 Lua 脚本保证原子性。

### 配置

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

声明式配置，内部自动注册 scheduler 任务：

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

### 实现机制

各队列类型使用不同的 Redis 数据结构：

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

超过 `max_retries` 的消息移入死信队列（独立 Stream）。

**消费者组协调：**

- 每个进程启动时注册为独立 consumer
- Stream 的消费者组保证每条消息只被一个 consumer 处理
- 进程退出后，其 pending 消息超过 idle 时间后可被其他 consumer 认领（`XCLAIM`）

### 配置

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

业务级分布式限流，支持全服统一配额。与网关层连接级限流（`rate_limit`，见 [安全语义](runtime-security.md#速率限制网关层)）不同：

| 层级 | 作用 | 存储 | 适用场景 |
|------|------|------|----------|
| 网关层 `rate_limit` | 按客户端 IP/连接限流 | 内存 | 防刷、防 DDoS |
| 业务层 `shield.rate_limiter()` | 按业务 key 限流 | 本地+Redis | 全服限流、API 限流 |

### 实现策略：本地令牌桶 + Redis 周期同步

每次 `allow()` 不请求 Redis，使用本地令牌桶纳秒级判断。周期性与 Redis 同步全局配额。

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

**精度特性：**

| 指标 | 值 |
|------|------|
| 判断延迟 | < 1μs（本地内存） |
| 同步延迟 | 1-5ms（Redis round-trip） |
| 精度误差 | < sync_interval（默认 1s，误差 < 1 秒内请求量） |
| 最终一致性 | 同步后全局配额精确 |

### Lua API

```lua
local limiter = shield.rate_limiter("api_limit", {
    rate = 100,                 -- 全局每秒 100 请求
    burst = 200,                -- 突发容量 200
})

-- 检查（本地判断，不请求 Redis）
local allowed = limiter:allow("client_ip")
local remaining = limiter:remaining("client_ip")

-- 阻塞等待（本地等待，不请求 Redis）
limiter:wait("client_ip", 5000)
```

### 滑动窗口

滑动窗口模式使用 Redis 计数，每次 `allow()` 需要 1 次 Redis 调用。适合低频精确限流场景：

```lua
local limiter = shield.rate_limiter("api_strict", {
    window = 60000,             -- 1 分钟窗口
    max_requests = 100,         -- 窗口内最多 100 次
    sliding = true,             -- 滑动窗口（需要 Redis）
})
```

### 算法对比

| 算法 | 每次请求 Redis | 精度 | 适用场景 |
|------|---------------|------|----------|
| `token_bucket` | 否（周期同步） | 近似 | 高频业务限流（默认） |
| `sliding_window` | 是（1 次 EVAL） | 精确 | 低频精确限流 |

### 配置

```yaml
rate_limiter:
  enabled: true
  algorithm: "token_bucket"    # token_bucket | sliding_window
  sync_interval: 1000          # 本地与 Redis 同步间隔（ms），仅 token_bucket
  cleanup_interval: 60000      # 过期 key 清理间隔（ms）
```

### 降级策略

| 场景 | token_bucket 行为 | sliding_window 行为 |
|------|-------------------|---------------------|
| Redis 不可用 | 继续使用本地配额，日志告警 | 拒绝所有请求（无法计数） |
| Redis 恢复 | 下次同步自动恢复全局配额 | 自动恢复 |
| 网络延迟高 | 同步延迟，本地配额可能偏大/偏小 | 请求延迟增加 |

token_bucket 模式下 Redis 故障不影响业务可用性，只影响全局配额精度。

---

## 六、Pub/Sub

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

```yaml
global:
  enabled: true
  redis:
    inherit: true
    db: 1
    prefix: "shield:global:"

  cache:
    enabled: true
    max_size: 10000
    default_ttl: 60000
    sync: true

lock:
  enabled: true
  default_ttl: 30000
  reentrant: true

rank:
  enabled: true
  max_members: 10000

queue:
  enabled: true
  defaults:
    max_length: 100000
    message_ttl: 86400000

rate_limiter:
  enabled: true
  algorithm: "token_bucket"
```

---

## 使用示例

### 全服BOSS

```lua
local lock = shield.lock()

function M.spawn_boss()
    local l = lock:acquire("boss_spawn", { ttl = 60000, reentrant = true })
    if not l then return nil, "busy" end

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

q:consume("reward_worker", function(msg)
    give_reward(msg.uid, msg.reward)
end)
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

-- 带时区
scheduler:cron("task", "0 0 * * *", fn, {
    timezone = "Asia/Shanghai",
})
```

### 任务管理

```lua
-- 获取任务
local task = scheduler:get("daily_reset")

-- 任务信息
task:name()                   -- 任务名
task:schedule()               -- cron 表达式或间隔
task:next_run()               -- 下次执行时间
task:last_run()               -- 上次执行时间
task:run_count()              -- 执行次数
task:status()                 -- "active" | "paused" | "error"

-- 暂停/恢复
scheduler:pause("daily_reset")
scheduler:resume("daily_reset")

-- 删除任务
scheduler:remove("daily_reset")

-- 手动触发
scheduler:trigger("daily_reset")
```

### 分布式调度

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
end, { distributed = true })
```

**定时活动：**

```lua
local scheduler = shield.scheduler()
local g = shield.global()

-- 每周六 20:00 开启双倍经验活动
scheduler:cron("double_exp_event", "0 20 * * 6", function()
    g:set("activity.double_exp", {
        status = "active",
        start_time = os.time(),
        end_time = os.time() + 7200,  -- 2小时
        multiplier = 2,
    })
    g:publish("activity_started", { name = "double_exp" })
end, { distributed = true })

-- 每周六 22:00 关闭活动
scheduler:cron("double_exp_end", "0 22 * * 6", function()
    g:set("activity.double_exp", { status = "inactive" })
    g:publish("activity_ended", { name = "double_exp" })
end, { distributed = true })
```

**数据备份：**

```lua
local scheduler = shield.scheduler()

-- 每天凌晨 3 点备份数据
scheduler:cron("data_backup", "0 3 * * *", function()
    backup_player_data()
    backup_global_data()
    shield.log.info("data backup completed")
end, { distributed = true })
```

### ops 暴露

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
| 全局数据 + 本地缓存 | P0 | 核心功能 |
| 分布式锁（重入、续期） | P0 | 全局操作必需 |
| 排行榜 | P0 | 游戏标配 |
| 普通队列 | P0 | 异步任务 |
| 延迟队列 | P0 | 延迟奖励、定时任务 |
| 可靠队列 | P0 | 关键业务 |
| 定时任务调度 | P0 | 每日重置、定时活动 |
| 限流器 | P1 | 防刷、防滥用 |
| Pub/Sub | P1 | 跨进程通知 |
| 优先级队列 | P1 | 紧急任务 |
| 广播队列 | P1 | 事件通知 |
| Redlock | P2 | 强一致场景 |
