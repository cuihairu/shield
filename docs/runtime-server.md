# 服务器状态运行时语义

本文档包含 Shield 服务器级别状态管理、全局共享数据相关的运行时语义决策。

## 设计原则

- ServerManager 是全局单例，管理服务器级别状态。
- 服务器状态与玩家状态分离。
- 全局数据通过 ServerManager 共享，避免广播同步。
- 服务器状态变更需要通知所有相关服务。

## ServerManager

ServerManager 是全局单例 Service，负责管理服务器级别的状态和数据。

### 职责

| 职责 | 说明 |
|------|------|
| 服务器状态 | 运行、维护、关闭等状态管理 |
| 全局计数器 | 总登录数、总消息数、总在线时长等 |
| 全局共享数据 | 世界BOSS、排行榜、活动状态等 |
| 运行时信息 | 启动时间、版本、节点ID等 |
| 配置快照 | 运行时配置查询 |
| 状态变更通知 | 服务器状态变更时通知其他服务 |

### 架构

```
┌─────────────────────────────────────────────────────────┐
│                  ServerManager (单例)                    │
│  ┌────────────────────────────────────────────────────┐ │
│  │  服务器状态                                          │ │
│  │  - state: running / maintenance / shutdown         │ │
│  │  - started_at: 启动时间                             │ │
│  │  - version: 版本号                                  │ │
│  │  - node_id: 节点ID                                  │ │
│  └────────────────────────────────────────────────────┘ │
│  ┌────────────────────────────────────────────────────┐ │
│  │  全局计数器                                          │ │
│  │  - total_logins: 总登录次数                          │ │
│  │  - total_messages: 总消息数                          │ │
│  │  - peak_online: 历史最高在线                          │ │
│  └────────────────────────────────────────────────────┘ │
│  ┌────────────────────────────────────────────────────┐ │
│  │  全局共享数据                                        │ │
│  │  - world_boss: { hp, status, ... }                 │ │
│  │  - rankings: { ... }                               │ │
│  │  - activities: { ... }                             │ │
│  └────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────┤
│  PlayerManager │ Services │ Ops                         │
└─────────────────────────────────────────────────────────┘
```

### Lua API

```lua
-- 获取 ServerManager
local server = shield.server()

-- 服务器状态
local state = server:state()                -- "running" | "maintenance" | "shutdown"
local uptime = server:uptime()              -- 运行时长（秒）
local version = server:version()            -- 版本号
local node_id = server:node_id()            -- 节点ID
local started_at = server:started_at()      -- 启动时间戳

-- 服务器状态控制
server:set_state("maintenance")             -- 设置状态
server:shutdown(30000)                      -- 30秒后关闭

-- 全局计数器
local count = server:counter("total_logins")
server:counter_inc("total_logins", 1)
server:counter_set("total_logins", 0)

-- 全局共享数据
local boss = server:get("world_boss")
server:set("world_boss", { hp = 1000, status = "alive" })
server:delete("world_boss")

-- 原子操作（用于排行榜等）
server:incr("total_logins")
server:decr("total_logins")
server:compare_and_set("world_boss", old_value, new_value)
```

### 状态机

```
┌────────────┐
│  starting  │
└─────┬──────┘
      │ init complete
      ▼
┌────────────┐
│  running   │◄─────────────┐
└─────┬──────┘              │
      │ maintenance         │
      ▼                     │
┌────────────┐              │
│maintenance │──────────────┘
└─────┬──────┘   resume
      │ shutdown
      ▼
┌────────────┐
│  shutdown  │
└────────────┘
```

### 状态说明

| 状态 | 说明 | 行为 |
|------|------|------|
| `starting` | 启动中 | 不接受玩家连接 |
| `running` | 正常运行 | 接受所有请求 |
| `maintenance` | 维护模式 | 不接受新登录，现有玩家可继续 |
| `shutdown` | 关闭中 | 通知所有玩家，准备关闭 |

### 配置

```yaml
# ServerManager 配置
server_manager:
  enabled: true
  name: "server_manager"               # 服务名称
  state: "running"                     # 初始状态

  # 服务器信息
  info:
    name: "My Game Server"
    version: "1.0.0"
    region: "us-east"

  # 全局计数器
  counters:
    - "total_logins"
    - "total_messages"
    - "peak_online"

  # 全局共享数据（带 TTL）
  shared_data:
    world_boss:
      ttl: 3600000                     # 1小时过期
    rankings:
      ttl: 60000                       # 1分钟过期

  # 状态变更钩子
  on_state_change: true                # 是否通知其他服务
```

### 与其他 Service 的交互

```lua
-- 其他 Service 查询服务器状态
local state = shield.call("server_manager", "state")
if state == "maintenance" then
    return nil, "server_under_maintenance"
end

-- 其他 Service 更新全局数据
shield.send("server_manager", "set", {
    key = "world_boss",
    value = { hp = 999, status = "alive" },
})

-- 其他 Service 监听状态变更
function M.on_server_state_change(new_state)
    if new_state == "shutdown" then
        -- 准备关闭
        M:save_all_data()
    end
end
```

### 服务器状态变更钩子

```lua
-- ServerManager 状态变更时通知所有服务
function M.set_state(new_state)
    local old_state = M.state
    M.state = new_state

    -- 通知所有注册的服务
    for _, service in ipairs(M.watchers) do
        shield.send(service, "on_server_state_change", {
            old_state = old_state,
            new_state = new_state,
        })
    end
end

-- 其他服务注册监听
function M.on_init(args)
    shield.call("server_manager", "watch_state", shield.self())
end
```

## 全局计数器

### 计数器类型

| 类型 | 说明 | 示例 |
|------|------|------|
| 累计计数器 | 只增不减 | 总登录数、总消息数 |
| 峰值计数器 | 记录历史最大值 | 历史最高在线 |
| 当前值计数器 | 可增可减 | 当前在线数 |

### 原子操作

```lua
-- 原子递增
local new_value = server:counter_inc("total_logins", 1)

-- 原子递减
local new_value = server:counter_dec("current_online", 1)

-- 原子比较并设置
local success = server:counter_cas("peak_online", old_peak, new_peak)

-- 批量获取
local counters = server:counters_get({"total_logins", "current_online", "peak_online"})
```

### 计数器持久化

```yaml
server_manager:
  counters_persistence:
    enabled: true
    storage: redis                     # memory | redis | database
    sync_interval: 60000               # 同步间隔（ms）
```

## 全局共享数据

### 数据类型

```lua
-- 简单键值对
server:set("maintenance_message", "Server will restart in 5 minutes")
local msg = server:get("maintenance_message")

-- 复杂对象
server:set("world_boss", {
    id = 1,
    name = "Dragon",
    hp = 1000,
    max_hp = 1000,
    status = "alive",
    last_killed_at = nil,
})

-- 带 TTL 的数据
server:set("daily_bonus", { claimed = false }, 86400000)  -- 24小时过期
```

### 数据同步

```yaml
server_manager:
  shared_data_sync:
    enabled: true
    strategy: "eager"                  # eager | lazy
    # eager: 立即通知所有服务
    # lazy: 服务查询时返回最新值

    # 数据变更通知
    watchers:
      world_boss:
        - "combat_service"
        - "ui_service"
      rankings:
        - "leaderboard_service"
```

### 排行榜实现

```lua
-- 使用 ServerManager 实现排行榜
function M.update_ranking(uid, score)
    local rankings = server:get("rankings") or {}

    -- 更新分数
    rankings[uid] = score

    -- 排序
    local sorted = {}
    for uid, score in pairs(rankings) do
        table.insert(sorted, { uid = uid, score = score })
    end
    table.sort(sorted, function(a, b) return a.score > b.score end)

    -- 只保留前100
    local top100 = {}
    for i = 1, math.min(100, #sorted) do
        table.insert(top100, sorted[i])
    end

    server:set("rankings", top100)
end

-- 查询排行榜
function M.get_rankings()
    return server:get("rankings") or {}
end
```

## ops 暴露

```json
GET /ops/server

{
  "state": "running",
  "uptime": 3600,
  "version": "1.0.0",
  "node_id": "node-1",
  "started_at": "2026-06-10T12:00:00Z",
  "counters": {
    "total_logins": 12345,
    "current_online": 5000,
    "peak_online": 8000,
    "total_messages": 9876543
  },
  "shared_data": {
    "world_boss": {
      "hp": 1000,
      "status": "alive"
    },
    "rankings_count": 100
  }
}
```

## 与其他 Manager 的关系

```
┌─────────────────────────────────────────────────────────┐
│                    ServerManager                         │
│  - 服务器状态                                            │
│  - 全局计数器                                            │
│  - 全局共享数据                                          │
├─────────────────────────────────────────────────────────┤
│  PlayerManager          │  OpsManager                   │
│  - 玩家管理              │  - 运维管理                    │
├─────────────────────────────────────────────────────────┤
│  Services                                               │
│  - 业务服务                                             │
└─────────────────────────────────────────────────────────┘
```

## 跨进程共享数据（GlobalData）

多进程部署时，ServerManager 的数据只在单进程内。需要 GlobalData 提供跨进程共享能力。

### 架构

```
┌─────────────────────────────────────────────────────────┐
│                    进程 1 (Node-1)                       │
│  ServerManager ←→ GlobalData (本地缓存)                  │
├─────────────────────────────────────────────────────────┤
│                    进程 2 (Node-2)                       │
│  ServerManager ←→ GlobalData (本地缓存)                  │
├─────────────────────────────────────────────────────────┤
│                    进程 3 (Node-3)                       │
│  ServerManager ←→ GlobalData (本地缓存)                  │
└─────────────┬───────────────────────────────┬───────────┘
              │                               │
              ▼                               ▼
    ┌─────────────────┐             ┌─────────────────┐
    │     Redis       │             │   Database      │
    │  (高频读写)      │             │  (持久化)        │
    └─────────────────┘             └─────────────────┘
```

### GlobalData vs ServerManager

| 维度 | ServerManager | GlobalData |
|------|---------------|------------|
| 作用域 | 单进程 | 跨进程 |
| 存储 | 内存 | Redis/DB |
| 延迟 | 纳秒 | 毫秒 |
| 用途 | 本地状态 | 全局共享 |
| 持久化 | 可选 | 必须 |

### 数据分类

```
┌─────────────────────────────────────────────────────────┐
│                    GlobalData                            │
├─────────────────────────────────────────────────────────┤
│  基础数据 (BaseData) - 只读                               │
│  - 服务器配置（启动时加载）                                │
│  - 公告、活动配置                                         │
│  - 版本信息                                              │
│  - 热更新配置（运维推送）                                  │
├─────────────────────────────────────────────────────────┤
│  全局状态 (GlobalState) - 可读写                          │
│  - 世界BOSS状态                                          │
│  - 跨服排行榜                                            │
│  - 全局活动状态（开启/关闭/进度）                          │
│  - 全服公告                                              │
├─────────────────────────────────────────────────────────┤
│  全局计数器 (GlobalCounters) - 原子操作                   │
│  - 全服总登录数                                           │
│  - 全服总在线数                                           │
│  - 全服峰值在线                                           │
│  - 活动参与人数                                           │
├─────────────────────────────────────────────────────────┤
│  分布式锁 (DistributedLock)                              │
│  - 全局唯一操作锁（BOSS生成、活动开启）                    │
│  - 定时任务锁（防止多进程重复执行）                        │
└─────────────────────────────────────────────────────────┘
```

### 热更新配置

运维可以通过 GlobalData 推送配置更新，所有进程实时生效：

```lua
-- 运维推送配置更新
gd.set("config.hotfix", {
    drop_rate = 2.0,        -- 掉率翻倍
    exp_bonus = 1.5,        -- 经验加成
    maintenance = false,    -- 维护模式
}, 3600000)                 -- 1小时后过期

-- 业务服务读取热更新配置
function M.get_drop_rate()
    local hotfix = gd.get("config.hotfix")
    if hotfix and hotfix.drop_rate then
        return hotfix.drop_rate
    end
    return config.default_drop_rate
end
```

### 全服活动状态

```lua
-- 活动状态管理
gd.set("activity.summer_festival", {
    status = "active",          -- inactive | active | ended
    start_time = os.time(),
    end_time = os.time() + 86400,
    progress = 0,               -- 全服进度
    target = 100000,            -- 目标
    rewards_claimed = false,
})

-- 所有进程读取活动状态
function M.check_activity()
    local activity = gd.get("activity.summer_festival")
    if activity and activity.status == "active" then
        return true, activity
    end
    return false, nil
end

-- 更新活动进度（原子操作）
function M.update_activity_progress(amount)
    gd.counter_inc("activity.summer_festival.progress", amount)
end
```

### Lua API

```lua
-- 获取 GlobalData
local gd = shield.global_data()

-- 基础数据（只读，启动时加载）
local server_name = gd.base("server_name")
local version = gd.base("version")

-- 全局状态（可读写）
local boss = gd.get("world_boss")
gd.set("world_boss", { hp = 1000, status = "alive" })
gd.delete("world_boss")

-- 全局计数器（原子操作）
local total = gd.counter("total_logins")
gd.counter_inc("total_logins", 1)
gd.counter_dec("total_logins", 1)
gd.counter_set("total_logins", 0)

-- 分布式锁
local lock = gd.lock("boss_spawn_lock", 30000)  -- 30秒超时
if lock then
    -- 获取锁成功
    spawn_boss()
    lock:release()
end

-- 批量操作
local data = gd.mget({"world_boss", "rankings", "daily_bonus"})
gd.mset({
    world_boss = { hp = 1000 },
    rankings = { ... },
})
```

### 配置

```yaml
# GlobalData 配置
global_data:
  enabled: true

  # 基础数据（只读，启动时从配置加载）
  base:
    server_name: "My Game Server"
    version: "1.0.0"
    region: "us-east"
    max_online: 50000

  # 存储后端
  storage:
    type: redis                     # redis | database
    redis:
      host: localhost
      port: 6379
      db: 1
      prefix: "shield:global:"
    database:
      driver: mysql
      table: "global_data"

  # 缓存策略
  cache:
    enabled: true
    ttl: 60000                      # 本地缓存 TTL（ms）
    max_size: 1000                  # 最大缓存条目数

  # 数据同步
  sync:
    strategy: "pubsub"              # pubsub | polling
    # pubsub: 通过 Redis Pub/Sub 实时同步
    # polling: 定期轮询检查变更
    poll_interval: 5000             # 轮询间隔（ms）

  # 全局计数器
  counters:
    - "total_logins"
    - "total_messages"
    - "peak_online"
    - "total_online_time"

  # 分布式锁
  lock:
    enabled: true
    ttl: 30000                      # 锁默认超时（ms）
    retry_interval: 100             # 获取锁重试间隔（ms）
    max_retries: 100                # 最大重试次数
```

### 数据同步机制

**Pub/Sub 模式（推荐）：**

```
Node-1                    Redis                    Node-2
   │                        │                        │
   │  SET world_boss        │                        │
   │──────────────────────►│                        │
   │                        │  PUBLISH global:world_boss
   │                        │──────────────────────►│
   │                        │                        │
   │                        │        更新本地缓存     │
```

**Polling 模式：**

```
Node-1                    Redis                    Node-2
   │                        │                        │
   │  GET world_boss        │                        │
   │──────────────────────►│                        │
   │  返回数据 + version    │                        │
   │◄──────────────────────│                        │
   │                        │                        │
   │  每5秒检查 version     │                        │
   │  变更则更新本地缓存     │                        │
```

### 与其他 Service 的交互

```lua
-- 查询全局数据
local boss = shield.call("global_data", "get", "world_boss")

-- 更新全局数据
shield.send("global_data", "set", {
    key = "world_boss",
    value = { hp = 999, status = "alive" },
})

-- 获取全局计数器
local total = shield.call("global_data", "counter", "total_logins")

-- 获取分布式锁
local lock = shield.call("global_data", "lock", {
    key = "boss_spawn_lock",
    ttl = 30000,
})
```

### 全局排行榜实现

```lua
-- 使用 GlobalData 实现跨服排行榜
function M.update_ranking(uid, score)
    -- 原子更新排行榜
    local rankings = gd.get("rankings") or {}

    -- 更新分数
    rankings[uid] = score

    -- 排序并保留前100
    local sorted = {}
    for uid, score in pairs(rankings) do
        table.insert(sorted, { uid = uid, score = score })
    end
    table.sort(sorted, function(a, b) return a.score > b.score end)

    local top100 = {}
    for i = 1, math.min(100, #sorted) do
        table.insert(top100, sorted[i])
    end

    gd.set("rankings", top100)
end

-- 查询全服排行榜
function M.get_rankings()
    return gd.get("rankings") or {}
end
```

### 全局BOSS实现

```lua
-- 使用 GlobalData + 分布式锁实现全服BOSS
function M.spawn_boss()
    -- 获取锁，确保只有一个进程生成BOSS
    local lock = gd.lock("boss_spawn_lock", 30000)
    if not lock then
        return nil, "another_process_spawning"
    end

    -- 检查BOSS是否已存在
    local boss = gd.get("world_boss")
    if boss and boss.status == "alive" then
        lock:release()
        return nil, "boss_already_alive"
    end

    -- 生成BOSS
    local new_boss = {
        id = 1,
        name = "Dragon",
        hp = 1000,
        max_hp = 1000,
        status = "alive",
        spawned_at = os.time(),
        spawned_by = shield.server():node_id(),
    }

    gd.set("world_boss", new_boss)
    lock:release()

    return new_boss
end

-- 攻击BOSS
function M.attack_boss(uid, damage)
    -- 使用乐观锁更新BOSS血量
    local boss = gd.get("world_boss")
    if not boss or boss.status ~= "alive" then
        return nil, "boss_not_alive"
    end

    local old_hp = boss.hp
    local new_hp = math.max(0, old_hp - damage)

    boss.hp = new_hp
    if new_hp <= 0 then
        boss.status = "dead"
        boss.killed_by = uid
        boss.killed_at = os.time()
    end

    -- CAS 更新
    local success = gd.cas("world_boss",
        { hp = old_hp, status = "alive" },
        boss
    )

    if success then
        if new_hp <= 0 then
            -- BOSS 被击杀，广播全服
            gd.publish("boss_killed", { uid = uid })
        end
        return { damage = damage, boss_hp = new_hp }
    else
        -- 并发冲突，重试
        return M.attack_boss(uid, damage)
    end
end
```

### ops 暴露

```json
GET /ops/global-data

{
  "storage": {
    "type": "redis",
    "connected": true,
    "latency_ms": 2
  },
  "cache": {
    "size": 150,
    "hit_rate": 0.95,
    "miss_rate": 0.05
  },
  "data": {
    "world_boss": {
      "hp": 1000,
      "status": "alive",
      "updated_at": "2026-06-10T12:00:00Z"
    },
    "rankings_count": 100,
    "total_logins": 123456
  },
  "locks": {
    "active": 2,
    "waiting": 0
  }
}
```

## 实现优先级

| 功能 | 优先级 | 说明 |
|------|--------|------|
| 服务器状态管理 | P0 | 维护模式、关闭流程 |
| 全局计数器 | P0 | 统计需求 |
| 运行时信息查询 | P0 | ops 集成 |
| 全局共享数据 | P1 | 世界BOSS、排行榜 |
| 状态变更通知 | P1 | 服务联动 |
| 跨进程 GlobalData | P1 | 多进程部署必需 |
| 分布式锁 | P1 | 全局唯一操作 |
| 计数器持久化 | P2 | 数据安全 |
