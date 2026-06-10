# 全局能力运行时语义

本文档包含 Shield 跨进程共享数据、分布式锁、排行榜、消息队列等全局能力的运行时语义决策。

## 设计原则

- 深度集成 Redis，封装常用游戏模式。
- 命名空间分离，职责清晰。
- 提供本地缓存，减少 Redis 压力。
- 统一接口，底层可切换。

## 命名空间

```lua
shield.global()           -- 全局数据（KV、缓存）
shield.lock()             -- 分布式锁
shield.rank()             -- 排行榜
shield.queue()            -- 消息队列
shield.rate_limiter()     -- 限流器
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

```lua
local daily_rank = shield.rank("daily_score", {
    reset = "daily",          -- daily | weekly | monthly
    reset_time = "00:00",
    archive = true,           -- 归档旧数据
})
```

### 配置

```yaml
rank:
  enabled: true
  max_members: 10000
  reset:
    enabled: true
    archive_table: "rank_archive"
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

```lua
local limiter = shield.rate_limiter("api_limit", {
    rate = 100,                 -- 每秒100请求
    burst = 200,                -- 突发200
})

-- 检查
local allowed = limiter:allow("client_ip")
local remaining = limiter:remaining("client_ip")

-- 等待
limiter:wait("client_ip", 5000)
```

### 滑动窗口

```lua
local limiter = shield.rate_limiter("sliding", {
    window = 60000,             -- 1分钟窗口
    max_requests = 100,
    sliding = true,
})
```

### 配置

```yaml
rate_limiter:
  enabled: true
  algorithm: "token_bucket"    # token_bucket | sliding_window
  cleanup_interval: 60000
```

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

## 实现优先级

| 功能 | 优先级 | 说明 |
|------|--------|------|
| 全局数据 + 本地缓存 | P0 | 核心功能 |
| 分布式锁（重入、续期） | P0 | 全局操作必需 |
| 排行榜 | P0 | 游戏标配 |
| 普通队列 | P0 | 异步任务 |
| 延迟队列 | P0 | 延迟奖励、定时任务 |
| 可靠队列 | P0 | 关键业务 |
| 限流器 | P1 | 防刷、防滥用 |
| Pub/Sub | P1 | 跨进程通知 |
| 优先级队列 | P1 | 紧急任务 |
| 广播队列 | P1 | 事件通知 |
| Redlock | P2 | 强一致场景 |
