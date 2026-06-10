# 数据访问运行时语义

本文档包含 Shield 数据访问相关的运行时语义决策。

## shield_data

`shield_data` 提供原始 DB/Redis 能力，不做 ORM。

```lua
local ok, rows = shield.db.query("SELECT * FROM users WHERE id = ?", { uid })
local ok, result = shield.redis.get("player:" .. uid)
```

规则：

- API coroutine-friendly。
- query 不阻塞 runtime 线程。
- 返回 `ok, result_or_error`。
- 支持超时。
- 支持连接池。
- 不做分布式事务。
- 不跨 service 自动共享 transaction。

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
  query_timeout: 30000       # 查询超时（ms）
  idle_timeout: 300000       # 空闲连接超时（ms）
  max_lifetime: 3600000      # 连接最大生命周期（ms）

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
  command_timeout: 5000      # 命令超时（ms）
  idle_timeout: 300000       # 空闲连接超时（ms）

  # 哨兵/集群配置（可选）
  sentinel:
    master_name: "mymaster"
    addresses:
      - "127.0.0.1:26379"
      - "127.0.0.1:26380"
```

### 连接池行为

```
┌─────────────────────────────────────────────────────────┐
│  连接请求                                                │
│  - 优先使用空闲连接                                       │
│  - 无空闲且 < max_pool_size: 创建新连接                   │
│  - 达到 max_pool_size: 等待（带超时）                      │
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

```lua
local ok, err = shield.db.transaction(function(tx)
    -- tx 对象提供 query/execute 方法
    local ok, user = tx:query_one("SELECT * FROM users WHERE id = ? FOR UPDATE", { uid })
    if not user then
        return false, "user not found"
    end

    local ok, err = tx:execute("UPDATE users SET balance = balance - ? WHERE id = ?", { amount, uid })
    if not ok then
        return false, err
    end

    local ok, err = tx:execute("INSERT INTO logs (uid, action, amount) VALUES (?, ?, ?)", { uid, "deduct", amount })
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
- 事务超时使用 `query_timeout`。
- 事务失败自动回滚。
- 不支持嵌套事务（savepoint 由驱动决定）。
- 不跨 service 共享事务。

### 预编译语句

```lua
-- 预编译语句（可选，驱动支持时）
local stmt = shield.db.prepare("SELECT * FROM users WHERE id = ?")
local ok, row = stmt:query_one({ uid })
stmt:close()
```

## Redis API

### 基础命令

```lua
-- String
local ok, value = shield.redis.get("key")
local ok, result = shield.redis.set("key", "value", 3600)  -- TTL 可选
local ok, result = shield.redis.del("key")
local ok, result = shield.redis.exists("key")

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

```lua
local ok, result = shield.redis.eval(
    "return redis.call('set', KEYS[1], ARGV[1])",
    { "mykey" },      -- KEYS
    { "myvalue" }     -- ARGV
)
```

## 错误处理

### 数据库错误

```lua
local ok, result = shield.db.query("SELECT * FROM non_existent")
if not ok then
    -- result 是 Error 对象
    shield.log.error("db error: " .. result.code .. " - " .. result.message)

    if result.code == "connection_lost" then
        -- 连接丢失，下次查询会自动重连
    elseif result.code == "query_timeout" then
        -- 查询超时
    elseif result.code == "syntax_error" then
        -- SQL 语法错误
    end
end
```

数据库错误码：

```txt
connection_lost      # 连接丢失
connection_timeout   # 建立连接超时
query_timeout        # 查询超时
syntax_error         # SQL 语法错误
constraint_violation # 约束违反
transaction_aborted  # 事务中止
pool_exhausted       # 连接池耗尽
```

### Redis 错误

```lua
local ok, result = shield.redis.get("key")
if not ok then
    shield.log.error("redis error: " .. result.code .. " - " .. result.message)
end
```

Redis 错误码：

```txt
connection_lost      # 连接丢失
connection_timeout   # 建立连接超时
command_timeout      # 命令超时
wrong_type           # 类型错误
pool_exhausted       # 连接池耗尽
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
