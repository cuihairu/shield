# 全局数据运行时语义

本文档包含 Shield 跨进程共享数据、分布式锁、排行榜等全局能力的运行时语义决策。

## 设计原则

- 深度集成 Redis，封装常用游戏模式。
- 提供本地缓存，减少 Redis 压力。
- 统一接口，底层可切换。
- 内置分布式锁、排行榜、限流器等常用组件。

## 架构

```
┌─────────────────────────────────────────────────────────┐
│                  Lua 业务代码                            │
│  shield.global.*                                        │
├─────────────────────────────────────────────────────────┤
│                  shield_global                           │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐   │
│  │  Cache   │ │   Lock   │ │  Rank    │ │  Rate    │   │
│  │ 本地缓存  │ │ 分布式锁  │ │ 排行榜   │ │ 限流器   │   │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘   │
├─────────────────────────────────────────────────────────┤
│                  shield_data (Redis)                     │
│  ┌────────────────────────────────────────────────────┐ │
│  │  Connection Pool + Pub/Sub                         │ │
│  └────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

## 全局数据 API

### 基础 KV 操作

```lua
local g = shield.global()

-- 读写
g:set("key", value, ttl)    -- ttl 可选，毫秒
local value = g:get("key")
g:delete("key")
g:exists("key")

-- 批量操作
g:mset({ key1 = "v1", key2 = "v2" })
local values = g:mget("key1", "key2")

-- 原子操作
g:incr("counter", 1)         -- 原子递增
g:decr("counter", 1)         -- 原子递减
g:incr_by("counter", 10)     -- 原子增加指定值

-- 本地缓存（高频读取场景）
local value = g:get_cached("key", 60000)  -- 60秒缓存
g:invalidate("key")                       -- 手动失效缓存
```

### 本地缓存策略

```yaml
global:
  cache:
    enabled: true
    max_size: 10000           # 最大缓存条目
    default_ttl: 60000        # 默认缓存 TTL（ms）
    strategy: "ttl"           # ttl | lru | lfu
    sync:
      enabled: true           # 跨进程缓存同步
      channel: "shield:cache:invalidate"
```

**缓存同步流程：**

```
Node-1                    Redis                    Node-2
   │                        │                        │
   │  SET key value         │                        │
   │──────────────────────►│                        │
   │  PUBLISH cache:invalidate {key}                │
   │──────────────────────►│                        │
   │                        │  通知 Node-2           │
   │                        │──────────────────────►│
   │                        │                        │
   │                        │        失效本地缓存     │
```

## 分布式锁

### 完整实现

```lua
local g = shield.global()

-- 获取锁（阻塞等待）
local lock = g:lock("my_lock", {
    ttl = 30000,              -- 锁超时时间（ms）
    wait = 10000,             -- 最大等待时间（ms）
    retry = 100,              -- 重试间隔（ms）
    reentrant = true,         -- 是否可重入
})

-- 获取锁（非阻塞）
local lock = g:try_lock("my_lock", {
    ttl = 30000,
    reentrant = true,
})

if lock then
    -- 持有锁
    lock:extend(30000)        -- 续期
    lock:owner()              -- 获取持有者信息
    lock:ttl()                -- 获取剩余时间
    lock:release()            -- 释放锁
end
```

### 锁信息

```lua
local lock = g:lock("boss_spawn_lock", { ttl = 30000 })

-- 锁信息
{
    key = "boss_spawn_lock",
    owner = {
        node_id = "node-1",
        service_id = 12345,
        service_name = "boss_manager",
        thread_id = "coroutine-67890",
    },
    acquired_at = 1234567890000,
    ttl = 30000,
    reentrant_count = 1,      -- 重入次数
}
```

### 重入锁

```lua
-- 同一服务可以多次获取同一锁
local lock1 = g:lock("my_lock", { ttl = 30000, reentrant = true })
local lock2 = g:lock("my_lock", { ttl = 30000, reentrant = true })

-- lock1 和 lock2 是同一个锁对象
-- 必须释放相同次数
lock2:release()  -- reentrant_count = 1
lock1:release()  -- reentrant_count = 0，真正释放
```

### 锁续期

```lua
local lock = g:lock("long_task", { ttl = 30000 })

-- 自动续期（后台协程）
lock:auto_extend(true, 10000)  -- 每10秒续期一次

-- 执行长时间任务
do_long_task()

-- 关闭自动续期
lock:auto_extend(false)
lock:release()
```

### 公平锁

```lua
-- 公平锁：按请求顺序获取
local lock = g:lock("fair_lock", {
    ttl = 30000,
    fair = true,              -- 启用公平锁
    wait = 10000,
})
```

### Redlock（多节点锁）

```lua
-- Redlock：跨多个 Redis 节点的强一致锁
local lock = g:redlock("critical_lock", {
    ttl = 30000,
    quorum = 3,               -- 需要3个节点同意
    retry = 100,
    wait = 10000,
})
```

### 配置

```yaml
global:
  lock:
    enabled: true
    default_ttl: 30000        # 默认锁超时（ms）
    max_ttl: 300000           # 最大锁超时（ms）
    reentrant: true           # 默认启用重入
    auto_extend:
      enabled: true
      interval: 10000         # 续期间隔（ms）
    fair:
      enabled: false          # 默认关闭公平锁
    redlock:
      enabled: false          # 默认关闭 Redlock
      nodes: []               # Redis 节点列表
      quorum: 3
```

## 排行榜

### 基于 Redis Sorted Set

```lua
local g = shield.global()
local rank = g:rank("player_score")

-- 更新分数
rank:update("player_1", 1000)
rank:update("player_2", 950)
rank:update("player_3", 1100)

-- 批量更新
rank:mupdate({
    player_1 = 1000,
    player_2 = 950,
    player_3 = 1100,
})

-- 查询排名（从1开始）
local pos = rank:position("player_1")  -- 2

-- 查询分数
local score = rank:score("player_1")   -- 1000

-- Top N
local top10 = rank:top(10)
-- 返回: {{ uid = "player_3", score = 1100, rank = 1 }, ...}

-- 范围查询（按排名）
local range = rank:range(1, 100)       -- 排名 1-100

-- 范围查询（按分数）
local by_score = rank:range_by_score(900, 1100)

-- 查询玩家周围排名
local around = rank:around("player_1", 5)  -- 前后各5名

-- 总人数
local total = rank:count()

-- 删除
rank:remove("player_1")
rank:clear()
```

### 排行榜数据结构

```lua
-- top(10) 返回
{
    {
        uid = "player_3",
        score = 1100,
        rank = 1,
        member = { ... },    -- 可选的附加数据
    },
    {
        uid = "player_1",
        score = 1000,
        rank = 2,
    },
    -- ...
}

-- around("player_1", 5) 返回
{
    above = { ... },         -- 排名在上面的5个
    target = {               -- 目标玩家
        uid = "player_1",
        score = 1000,
        rank = 2,
    },
    below = { ... },         -- 排名在下面的5个
}
```

### 多维度排行榜

```lua
-- 支持多个排行榜
local score_rank = g:rank("player_score")
local level_rank = g:rank("player_level")
local vip_rank = g:rank("player_vip")

-- 更新多个排行榜
score_rank:update("player_1", 1000)
level_rank:update("player_1", 50)
vip_rank:update("player_1", 10)
```

### 排行榜定时重置

```lua
-- 每日重置排行榜
local daily_rank = g:rank("daily_score", {
    reset = "daily",          -- daily | weekly | monthly
    reset_time = "00:00",     -- 重置时间
    archive = true,           -- 是否归档旧数据
})
```

### 配置

```yaml
global:
  rank:
    enabled: true
    default_format: "sorted_set"  # sorted_set | list
    max_members: 10000            # 最大成员数
    reset:
      enabled: true
      archive_table: "rank_archive"
```

## 限流器

### 令牌桶算法

```lua
local g = shield.global()
local limiter = g:rate_limiter("api_limit", {
    rate = 100,               -- 每秒100个请求
    burst = 200,              -- 突发200个
})

-- 检查是否允许
local allowed = limiter:allow("client_ip")
if not allowed then
    return nil, "rate_limited"
end

-- 获取剩余配额
local remaining = limiter:remaining("client_ip")

-- 等待直到允许
limiter:wait("client_ip", 5000)  -- 最多等待5秒
```

### 滑动窗口

```lua
local limiter = g:rate_limiter("sliding_window", {
    window = 60000,           -- 1分钟窗口
    max_requests = 100,       -- 最多100次
    sliding = true,           -- 启用滑动窗口
})

local allowed = limiter:allow("user_123")
```

### 配置

```yaml
global:
  rate_limiter:
    enabled: true
    algorithm: "token_bucket"  # token_bucket | sliding_window
    cleanup_interval: 60000    # 清理过期条目间隔
```

## 分布式计数器

```lua
local g = shield.global()

-- 原子计数器
g:counter_set("total_logins", 0)
g:counter_inc("total_logins", 1)
g:counter_dec("total_logins", 1)
g:counter_add("total_logins", 100)
local value = g:counter_get("total_logins")

-- 批量计数器
g:counter_mset({
    total_logins = 0,
    total_messages = 0,
    peak_online = 0,
})
local values = g:counter_mget("total_logins", "total_messages", "peak_online")

-- 原子比较并设置
local success = g:counter_cas("peak_online", old_value, new_value)
```

### 计数器持久化

```yaml
global:
  counter:
    enabled: true
    persistence:
      enabled: true
      sync_interval: 60000    # 同步到 DB 间隔
      table: "global_counters"
```

## 发布/订阅

```lua
local g = shield.global()

-- 订阅频道
g:subscribe("system_notice", function(channel, message)
    shield.log.info("received: " .. message.text)
end)

-- 发布消息
g:publish("system_notice", {
    text = "Server maintenance in 5 minutes",
    type = "warning",
})

-- 模式订阅
g:psubscribe("player.*", function(channel, message)
    -- 处理 player.1, player.2 等频道
end)
```

## 配置示例

```yaml
global:
  enabled: true

  # Redis 连接（继承自 shield_data.redis）
  redis:
    inherit: true              # 继承全局 Redis 配置
    db: 1                      # 使用单独的 DB
    prefix: "shield:global:"   # Key 前缀

  # 本地缓存
  cache:
    enabled: true
    max_size: 10000
    default_ttl: 60000
    sync: true

  # 分布式锁
  lock:
    enabled: true
    default_ttl: 30000
    max_ttl: 300000
    reentrant: true
    auto_extend: true

  # 排行榜
  rank:
    enabled: true
    max_members: 10000

  # 限流器
  rate_limiter:
    enabled: true
    algorithm: "token_bucket"

  # 计数器
  counter:
    enabled: true
    persistence: true

  # Pub/Sub
  pubsub:
    enabled: true
```

## ops 暴露

```json
GET /ops/global

{
  "redis": {
    "connected": true,
    "latency_ms": 2,
    "db": 1
  },
  "cache": {
    "size": 1500,
    "hit_rate": 0.95,
    "miss_rate": 0.05,
    "evictions": 100
  },
  "locks": {
    "active": 5,
    "waiting": 2,
    "expired_today": 10
  },
  "ranks": {
    "count": 3,
    "total_members": 5000
  },
  "counters": {
    "total_logins": 123456,
    "peak_online": 8000
  },
  "rate_limiter": {
    "active_limiters": 10,
    "rejected_today": 500
  }
}
```

## 使用示例

### 全服BOSS

```lua
function M.spawn_boss()
    local lock = g:lock("boss_spawn", { ttl = 60000, reentrant = true })
    if not lock then
        return nil, "another_spawning"
    end

    local boss = g:get("world_boss")
    if boss and boss.status == "alive" then
        lock:release()
        return nil, "boss_alive"
    end

    g:set("world_boss", {
        hp = 10000,
        status = "alive",
        spawned_by = shield.server():node_id(),
    })

    lock:release()
    return true
end

function M.attack_boss(uid, damage)
    local lock = g:lock("boss_attack", { ttl = 5000 })
    if not lock then return nil, "busy" end

    local boss = g:get("world_boss")
    boss.hp = boss.hp - damage

    if boss.hp <= 0 then
        boss.status = "dead"
        boss.killed_by = uid
    end

    g:set("world_boss", boss)
    lock:release()

    if boss.hp <= 0 then
        g:publish("boss_killed", { uid = uid })
    end

    return { damage = damage, hp = boss.hp }
end
```

### 全服排行榜

```lua
local rank = g:rank("damage_rank")

function M.update_damage(uid, damage)
    local current = rank:score(uid) or 0
    if damage > current then
        rank:update(uid, damage)
    end
end

function M.get_top100()
    return rank:top(100)
end

function M.get_my_rank(uid)
    local pos = rank:position(uid)
    local score = rank:score(uid)
    local around = rank:around(uid, 5)
    return { rank = pos, score = score, around = around }
end
```

### API 限流

```lua
local limiter = g:rate_limiter("login_limit", {
    rate = 10,
    burst = 20,
})

function M.login(ip, credentials)
    if not limiter:allow(ip) then
        return nil, "too_many_requests"
    end

    -- 处理登录
    return do_login(credentials)
end
```

## 实现优先级

| 功能 | 优先级 | 说明 |
|------|--------|------|
| 基础 KV + 本地缓存 | P0 | 核心功能 |
| 分布式锁（重入、续期） | P0 | 全局操作必需 |
| 排行榜 | P0 | 游戏标配 |
| 分布式计数器 | P0 | 统计需求 |
| 限流器 | P1 | 防刷、防滥用 |
| Pub/Sub | P1 | 跨进程通知 |
| Redlock | P2 | 强一致场景 |
| 排行榜重置 | P2 | 定时任务 |
