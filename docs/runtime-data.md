# 数据访问运行时语义

> ⚠️ 状态：**本文档已严重过时，与当前代码不符，待重写。**
>
> 下文所述 `shield_data` / `DatabaseConnection` / `DatabasePool`、`shield.db.*` / `shield.redis.*` Lua API、"CTest 已覆盖"等实现描述，**在当前源码中均不存在**——核心 Lua 绑定未注册 db/redis 模块。数据访问已重构为**插件 namespace**，权威契约以 [Lua API 契约](lua-api.md) 为准（`shield.database.<driver>("name"):query(...)`；lua-api.md 的"删除的旧 API"已明确移除 `shield.db:*` / `shield.redis:*`）。阅读请以 lua-api.md 为唯一事实来源，本文保留仅为重写参考。

当前实现状态：数据库已改为**插件架构**。核心 `shield_data` 提供 `DatabaseConnection`
接口和 `DatabasePool` 连接池；具体后端（MySQL/PostgreSQL/SQLite）通过动态库插件
（`shield_db_mysql.dll` / `shield_db_pgsql.dll` / `shield_db_sqlite.dll`）在运行时加载。
插件实现 `db_plugin.h` 定义的 C ABI 接口，核心不直接链接任何数据库驱动。

配置通过 vcpkg features 控制：
- `database-mysql` — 安装 mysql-connector-cpp，构建 `shield_db_mysql` 插件
- `database-postgresql` — 安装 libpq，构建 `shield_db_pgsql` 插件
- `database-sqlite` — 安装 sqlite3，构建 `shield_db_sqlite` 插件

未启用时 Lua API 返回 `false, module_unavailable`。CTest 已覆盖 mock pool 下
`shield.db.query/query_one/execute`、`shield.redis.get/set/del/exists/publish/subscribe`
的基础返回形态，以及 pool size、动态扩容、acquire timeout 和非 mock 连接失败
不静默降级。`shield.db.transaction` 已提供本地单连接事务最小实现；
`shield.db.mapper/register_mapper/entity` 已提供 Lua 层轻量 mapper/entity helper。
数据 worker 线程池、订阅回调回投 worker、XML schema-mapper 生成器仍未完成。

本文档包含 Shield 数据访问相关的运行时语义决策。

## shield_data

`shield_data` 提供原始 DB/Redis 能力，不做重 ORM。`shield_lua` 在原始 API 上提供
轻量 Lua mapper/entity helper，用于显式 SQL 模板、命名参数绑定和简单实体 CRUD SQL
生成；它不是 ActiveRecord，不做对象图加载、migration 或跨 service 事务。

```lua
local ok, rows = shield.db.query("SELECT * FROM users WHERE id = ?", { uid })
local ok, result = shield.redis.get("player:" .. uid)
```

规则：

- API 返回形态保持 coroutine-friendly；当前实现仍同步执行，data worker pool 属于后续项。
- 返回 `ok, result_or_error`。
- 支持超时。
- 支持连接池。
- 不做分布式事务。
- 不跨 service 自动共享 transaction。
- 未启用 database/redis 时返回 `false, module_unavailable`。

当前最小契约只冻结以下 core 能力：

- `shield.db.query`
- `shield.db.query_one`
- `shield.db.execute`
- `shield.db.transaction`
- `shield.db.mapper`
- `shield.db.register_mapper`
- `shield.db.entity`
- `shield.redis.get/set/del/exists`
- `shield.redis.publish/subscribe`

以下内容属于 Phase 2+ 或驱动内部优化，可以在文档中先行收敛语义，但不作为 Phase 1 最小实现必达项，也不进入 `lua-api-tests.md` 的最小验收：

- 显式预编译语句 handle
- Redis pipeline
- Redis eval
- Redis sentinel/cluster 配置
- XML schema-mapper parser / descriptor generator

prepared statement 可以作为 driver 内部优化存在，但不能在 Phase 1 暴露为 Lua public API。

## 连接池配置

### 数据库连接池

```yaml
database:
  driver: mysql              # mysql | postgresql | sqlite
  host: localhost
  port: 3306
  database: game
  username: root
  password: ${DB_PASSWORD}

  # 连接池配置
  pool_size: 10              # 最小连接数
  max_pool_size: 50          # 最大连接数
  connect_timeout: 5000      # 建立连接超时（ms）
  acquire_timeout: 5000      # 等待可用连接超时（ms）
  query_timeout: 30000       # 查询超时（ms）
  idle_timeout: 300000       # 空闲连接超时（ms）
  max_lifetime: 3600000      # 连接最大生命周期（ms）

  # 测试/开发专用。生产环境不要开启 mock；连接失败默认 fail fast。
  mock: false
  allow_mock_fallback: false

  # 重连策略
  reconnect:
    enabled: true
    max_retries: 3           # 最大重试次数
    initial_delay: 1000      # 初始延迟（ms）
    max_delay: 30000         # 最大延迟（ms）
    multiplier: 2            # 退避倍数

  # 驱动专属配置
  options:
    charset: utf8mb4
```

### Redis 连接池

```yaml
redis:
  host: localhost
  port: 6379
  db: 0
  password: ${REDIS_PASSWORD}

  # 连接池配置
  pool_size: 10              # 最小连接数
  max_pool_size: 50          # 最大连接数
  connect_timeout: 5000      # 建立连接超时（ms）
  acquire_timeout: 5000      # 等待可用连接超时（ms）
  command_timeout: 5000      # 命令超时（ms）
  idle_timeout: 300000       # 空闲连接超时（ms）

  # 测试/开发专用。生产环境不要开启 mock；连接失败默认 fail fast。
  mock: false
  allow_mock_fallback: false

  # sentinel / cluster 配置属于 Phase 2+，不进入 Phase 1 schema
```

### 连接池行为

```
┌─────────────────────────────────────────────────────────┐
│  连接请求                                                │
│  - 优先使用空闲连接                                       │
│  - 无空闲且 < max_pool_size: 创建新连接                   │
│  - 达到 max_pool_size: 等待 acquire_timeout，失败返回 pool_exhausted │
├─────────────────────────────────────────────────────────┤
│  连接回收                                                │
│  - 空闲超过 idle_timeout: 关闭                           │
│  - 存活超过 max_lifetime: 关闭（下次使用时重建）            │
│  - 连接异常: 关闭并重建                                   │
└─────────────────────────────────────────────────────────┘
```

## 数据库 API

### 查询

```lua
-- 查询多行
local ok, rows = shield.db.query("SELECT * FROM users WHERE level > ?", { 10 })

-- 查询单行
local ok, row = shield.db.query_one("SELECT * FROM users WHERE id = ?", { uid })

-- 执行（INSERT/UPDATE/DELETE）
local ok, result = shield.db.execute(
    "INSERT INTO users (name, email) VALUES (?, ?)",
    { name, email }
)
-- result: { affected_rows = 1, insert_id = 123 }
```

### 事务

`shield.db.transaction(fn)` 已提供本地单连接事务最小实现。callback 接收 `tx`
对象，`tx.query/query_one/execute` 均在同一连接上执行。

```lua
local ok, err = shield.db.transaction(function(tx)
    local ok, user = tx.query_one("SELECT * FROM users WHERE id = ? FOR UPDATE", { uid })
    if not user then
        return false, "user not found"
    end

    local ok, err = tx.execute("UPDATE users SET balance = balance - ? WHERE id = ?", { amount, uid })
    if not ok then
        return false, err
    end

    local ok, err = tx.execute("INSERT INTO logs (uid, action, amount) VALUES (?, ?, ?)", { uid, "deduct", amount })
    if not ok then
        return false, err
    end

    return true  -- 提交事务
end)

if not ok then
    shield.log.error("transaction failed: " .. err)
end
```

事务规则：

- 事务内所有操作在同一连接上执行。
- callback 抛错或返回 `false, reason` 时自动 rollback。
- callback 返回非 false 时 commit，并把 callback 返回值转发给调用方。
- callback 返回后 `tx` 句柄关闭，继续使用返回 `transaction_closed`。
- 当前不支持嵌套事务或 savepoint。
- 不跨 service 共享事务。

### Lua Mapper

`shield.db.mapper(def)` 创建轻量 mapper facade。SQL 模板只支持 `#{name}` 或
`#{nested.path}` 命名参数，运行时会编译成 `?` 占位符和有序 params 数组。
`${name}` 原样替换和多语句 SQL 会被拒绝。

```lua
local PlayerMapper = shield.db.mapper({
    SelectProfile = {
        type = "select",
        one = true,
        sql = "SELECT player_id, nickname FROM player WHERE player_id = #{player_id}"
    },
    DebitGold = {
        type = "update",
        transaction = "required",
        sql = "UPDATE wallet SET gold = gold - #{amount} WHERE player_id = #{player_id}"
    }
})

local ok, profile = PlayerMapper:SelectProfile({ player_id = uid })
local ok, result = PlayerMapper:DebitGold({ player_id = uid, amount = 10 })
```

规则：

- statement `type` 支持 `select`、`insert`、`update`、`delete`、`execute`。
- `select` 默认返回多行；设置 `one=true` 或 `result="one"` 时走 `query_one`。
- `transaction="required"` 在没有显式 tx 时自动用 `shield.db.transaction` 包一层。
- 传入事务句柄时复用当前连接：`PlayerMapper:DebitGold(tx, params)`。
- `shield.db.register_mapper("PlayerMapper", def)` 会把 mapper 挂到 `shield.db.PlayerMapper`。

### Lua Entity Helper

`shield.db.entity(def)` 是很薄的实体 SQL helper，只按白名单字段生成
`insert/update/delete/find` SQL，不跟踪对象生命周期。

```lua
local Player = shield.db.entity({
    table = "player",
    fields = { "player_id", "nickname", "level" },
    primary_key = "player_id"
})

local ok, inserted = Player:insert({ player_id = uid, nickname = name, level = 1 })
local ok, updated = Player:update({ player_id = uid, nickname = "neo" })
local ok, row = Player:find(uid)
local ok, removed = Player:delete(uid)
```

规则：

- `table`、字段名和 column 名必须是安全 SQL identifier。
- `fields` 是字段白名单；数组形式保留 SQL 字段顺序。
- `primary_key` 支持字符串或字符串数组。
- 不做 schema migration、脏字段跟踪、关联加载或级联保存。

### 预编译语句

显式预编译语句 handle 属于 Phase 2+。Phase 1 允许底层 driver 使用 prepared statement 优化 `query/query_one/execute`，但不向 Lua 暴露 `shield.db.prepare`。

```lua
-- 预编译语句（可选，驱动支持时）
local stmt = shield.db.prepare("SELECT * FROM users WHERE id = ?")
local ok, row = stmt:query_one({ uid })
stmt:close()
```

## Redis API

### Phase 1 基础命令

```lua
-- String
local ok, value = shield.redis.get("key")
local ok, result = shield.redis.set("key", "value", 3600)  -- TTL 可选
local ok, result = shield.redis.del("key")
local ok, result = shield.redis.exists("key")
```

Phase 1 只冻结 `get/set/del/exists` 与 `publish/subscribe`。Hash、List、Set、Sorted Set、`expire`、`ttl` 等命令属于 Phase 2+，不进入当前 `lua-api-tests.md` 最小验收。

```lua
-- Phase 2+ examples only

-- Hash
local ok, value = shield.redis.hget("hash", "field")
local ok, result = shield.redis.hset("hash", "field", "value")
local ok, hash = shield.redis.hgetall("hash")

-- List
local ok, result = shield.redis.lpush("list", "value")
local ok, result = shield.redis.rpush("list", "value")
local ok, value = shield.redis.lpop("list")
local ok, values = shield.redis.lrange("list", 0, -1)

-- Set
local ok, result = shield.redis.sadd("set", "member")
local ok, result = shield.redis.srem("set", "member")
local ok, members = shield.redis.smembers("set")

-- Sorted Set
local ok, result = shield.redis.zadd("zset", 1.0, "member")
local ok, values = shield.redis.zrange("zset", 0, -1)

-- 通用
local ok, result = shield.redis.expire("key", 3600)
local ok, ttl = shield.redis.ttl("key")
```

### 发布/订阅

```lua
-- 订阅
shield.redis.subscribe("channel", function(channel, message)
    shield.log.info("received on " .. channel .. ": " .. message)
end)

-- 发布
local ok, receivers = shield.redis.publish("channel", "hello")
```

### Pipeline

Pipeline 属于 Phase 2+。Phase 1 不提供 `shield.redis.pipeline`。

```lua
local results = shield.redis.pipeline(function(pipe)
    pipe:set("key1", "value1")
    pipe:set("key2", "value2")
    pipe:get("key1")
    pipe:get("key2")
end)
-- results: { true, true, "value1", "value2" }
```

### Lua 脚本

Redis Lua 脚本属于 Phase 2+。Phase 1 不提供 `shield.redis.eval`。

```lua
local ok, result = shield.redis.eval(
    "return redis.call('set', KEYS[1], ARGV[1])",
    { "mykey" },      -- KEYS
    { "myvalue" }     -- ARGV
)
```

## 错误处理

数据库和 Redis 错误码见 [错误码参考](runtime-errors.md)。

```lua
local ok, result = shield.db.query("SELECT * FROM non_existent")
if not ok then
    shield.log.error("db error: " .. result.code .. " - " .. result.message)

    if result.code == "connection_lost" then
        -- 连接丢失，下次查询会自动重连
    elseif result.code == "query_timeout" then
        -- 查询超时
    end
end
```

```lua
local ok, result = shield.redis.get("key")
if not ok then
    shield.log.error("redis error: " .. result.code .. " - " .. result.message)
end
```

## data ownership

DB/Redis 连接由 `shield_data` 管理，不由普通 Lua service 直接持有底层连接。

普通 service 只能通过 `shield.db.*` / `shield.redis.*` 调用。

ops 需要暴露：

- pool size。
- in-flight query。
- query latency。
- timeout count。
- reconnect count。
