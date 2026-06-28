# redis.driver

> Shield 官方 Redis 底层驱动插件。统一持有 redis++ 客户端、连接池和连接模式配置，为 `cache.redis`、`queue.redis`、`leaderboard.redis` 等上层插件提供 `shield.redis.v1` typed Redis 能力。

## 包信息

- **包 ID**: `redis.driver`
- **接口**: [`shield.redis.v1`](/plugin-system#interface-model)
- **Capabilities**: `typed-command`、`pipeline`、`raw-command`
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_PLUGIN_REDIS_DRIVER`
- **源码**: `plugins/redis.driver/`
- **依赖**: redis-plus-plus（redis++）、hiredis（通过 vcpkg）

## 构建启用

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_REDIS_DRIVER=ON
```

构建产物输出到 `<build>/plugins/redis.driver/`，包含 `manifest.yaml` 和 `bin/` 下的共享库。运行时由 host 扫描 `plugins.directory` 自动发现。

## 定位

`redis.driver` 不是业务语义插件。它封装 redis++ 的全部能力（连接池、typed 命令、pipeline、cluster、sentinel、TLS），对外暴露稳定 C ABI 的 `shield.redis.v1`。

业务侧优先使用上层能力插件：

| 上层插件 | 依赖接口 | 暴露接口 |
| --- | --- | --- |
| `cache.redis` | `shield.redis.v1` | `shield.cache.v1` |
| `queue.redis` | `shield.redis.v1` | `shield.queue.v1` |
| `leaderboard.redis` | `shield.redis.v1` | `shield.leaderboard.v1` |

这些插件不再各自创建 Redis 连接，而是通过插件依赖注入拿到 `shield.redis_v1*` vtable，直接调用 typed Redis 方法。

### 设计原则

- **redis++ 留在插件内部**：不跨 ABI 边界暴露 redis++ C++ 类型，避免 STL ABI、编译器 ABI、redis++ 版本耦合。
- **typed API 为主路径**：常用 Redis 能力（get、set、hset、zadd 等）走 typed 方法，编译期类型安全。
- **raw command 为逃生口**：不常用或 Redis 新版本命令走 `command(...)` fallback，保证能力全覆盖。
- **单一连接池**：所有 Redis 连接由 `redis.driver` 实例统一管理，避免上层插件各自建连导致连接数膨胀。
- **Lua binding 访问**：业务 Lua 使用 binding 逻辑名，不直接使用 instance id。

## 配置 Schema

配置通过 `plugins.instances[].config` 注入。

| 字段 | 类型 | 必填 | 默认值 | 说明 |
| --- | --- | --- | --- | --- |
| `mode` | string | 否 | `single` | 连接模式：`single`、`sentinel`、`cluster` |
| `host` | string | 是 | `127.0.0.1` | Redis 主机地址（`single` 模式） |
| `port` | integer | 否 | `6379` | Redis 端口，范围 1-65535 |
| `password` | string | 否 | - | 鉴权密码，`secret: true` 在日志中脱敏 |
| `db` | integer | 否 | `0` | Redis DB 索引，范围 0-15（`single` / `sentinel` 模式） |
| `pool_size` | integer | 否 | `8` | redis++ 连接池大小，范围 1-256 |
| `connect_timeout_ms` | integer | 否 | `5000` | 建连超时，范围 100-60000 毫秒 |
| `command_timeout_ms` | integer | 否 | `3000` | 单条命令超时，范围 100-60000 毫秒 |
| `tls.enabled` | boolean | 否 | `false` | 是否启用 TLS |
| `tls.cert_path` | string | 否 | - | TLS 客户端证书路径 |
| `tls.key_path` | string | 否 | - | TLS 客户端私钥路径 |
| `tls.ca_cert_path` | string | 否 | - | TLS CA 证书路径 |
| `sentinel.master_name` | string | 否 | - | Sentinel 主节点名（`sentinel` 模式必填） |
| `sentinel.password` | string | 否 | - | Sentinel 密码 |
| `cluster.addrs` | array | 否 | - | Cluster 节点地址列表（`cluster` 模式必填） |

完整 `app.yaml` 示例：

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: redis.main
      package: redis.driver
      required: true
      config:
        mode: single
        host: "127.0.0.1"
        port: 6379
        db: 0
        password: "${REDIS_PASSWORD:}"
        pool_size: 16
        connect_timeout_ms: 5000
        command_timeout_ms: 3000

    - id: cache.session
      package: cache.redis
      required: true
      dependencies:
        redis: redis.main

    - id: leaderboard.global
      package: leaderboard.redis
      required: true
      dependencies:
        redis: redis.main

  bindings:
    redis.default: redis.main
    cache.default: cache.session
    leaderboard.default: leaderboard.global
```

### Sentinel 模式

```yaml
- id: redis.main
  package: redis.driver
  config:
    mode: sentinel
    sentinel:
      master_name: mymaster
      password: "sentinel-pass"
    password: "redis-pass"
    db: 0
    pool_size: 16
```

### Cluster 模式

```yaml
- id: redis.main
  package: redis.driver
  config:
    mode: cluster
    cluster:
      addrs:
        - "10.0.0.1:6379"
        - "10.0.0.2:6379"
        - "10.0.0.3:6379"
    password: "redis-pass"
    pool_size: 16
```

## C ABI 接口

源文件：`include/shield/plugin/redis.h`。`redis.driver` 提供 `shield.redis.v1`，覆盖常用 Redis 数据结构操作，同时保留 raw command 逃生口。

### 值类型

```c
typedef enum shield_redis_type_v1 {
    SHIELD_REDIS_NIL     = 0,
    SHIELD_REDIS_STRING  = 1,
    SHIELD_REDIS_INTEGER = 2,
    SHIELD_REDIS_DOUBLE  = 3,
    SHIELD_REDIS_BOOL    = 4,
    SHIELD_REDIS_ARRAY   = 5,
    SHIELD_REDIS_ERROR   = 6
} shield_redis_type_v1;

typedef struct shield_redis_value_v1 {
    shield_redis_type_v1 type;
    const char* str;                        // STRING / ERROR
    uint64_t str_len;
    int64_t integer;                        // INTEGER
    double number;                          // DOUBLE
    int boolean;                            // BOOL
    struct shield_redis_value_v1* items;    // ARRAY
    uint64_t item_count;                    // ARRAY
} shield_redis_value_v1;
```

- `type` — 值的类型枚举。Redis `nil` 响应映射为 `SHIELD_REDIS_NIL`。
- `str` / `str_len` — STRING 和 ERROR 类型的字节内容，按长度读取而非 `\0` 截断。
- `integer` — Redis 整数响应（`INCR`、`LLEN` 等）。
- `number` — 浮点数响应（`ZSCORE` 等）。
- `boolean` — 布尔响应（`SETNX` 成功等）。
- `items` / `item_count` — ARRAY 类型的嵌套元素（`HGETALL`、`LRANGE` 等）。

### 命令参数

```c
typedef struct shield_redis_arg_v1 {
    const void* data;
    uint64_t len;
} shield_redis_arg_v1;

typedef struct shield_redis_command_v1 {
    const shield_redis_arg_v1* args;
    uint64_t argc;
} shield_redis_command_v1;
```

用于 `command()` 和 `pipeline()` 的参数传递。每个 `arg` 是一段字节，不要求 `\0` 终止。

### 主接口

```c
#define SHIELD_REDIS_V1 "shield.redis.v1"

typedef struct shield_redis_v1 {
    uint32_t struct_size;
    const char* interface_name;

    // Connection handle
    void* (*connect)(const void* cfg, char* err_buf, int err_buf_size);
    void  (*disconnect)(void* handle);

    // Key-Value
    int (*get)(void* inst, const char* key,
               shield_redis_value_v1** out, shield_error_v1* err);
    int (*set)(void* inst, const char* key, const char* val,
               int ttl_sec, shield_error_v1* err);
    int (*del)(void* inst, const char* key, shield_error_v1* err);

    // Hash
    int (*hget)(void* inst, const char* key, const char* field,
                shield_redis_value_v1** out, shield_error_v1* err);
    int (*hset)(void* inst, const char* key, const char* field,
                const char* val, shield_error_v1* err);
    int (*hgetall)(void* inst, const char* key,
                   shield_redis_value_v1** out, shield_error_v1* err);

    // Sorted Set
    int (*zadd)(void* inst, const char* key, double score,
                const char* member, shield_error_v1* err);
    int (*zrange)(void* inst, const char* key, int start, int stop,
                  shield_redis_value_v1** out, shield_error_v1* err);

    // Pipeline
    int (*pipeline)(void* inst, const shield_redis_command_v1* cmds,
                    uint64_t count, shield_redis_value_v1** out_array,
                    uint64_t* out_count, shield_error_v1* err);

    // Raw command (escape hatch)
    int (*command)(void* inst, const shield_redis_arg_v1* args,
                   uint64_t argc, shield_redis_value_v1** out,
                   shield_error_v1* err);

    // Memory management
    void (*free_value)(shield_redis_value_v1* value);
} shield_redis_v1;
```

### 方法说明

#### Connection Handle

| 方法 | 说明 |
| --- | --- |
| `connect` | 返回一个 opaque handle，供后续所有方法使用。`cfg` 可传 `NULL`（driver 使用自身实例配置）。失败返回 `NULL`，`err_buf` 写入错误信息。 |
| `disconnect` | 释放 handle。底层连接池由 driver 实例管理，不会被 disconnect 销毁。 |

#### Key-Value

| 方法 | Redis 命令 | 说明 |
| --- | --- | --- |
| `get` | `GET key` | 未命中返回 `SHIELD_REDIS_NIL` |
| `set` | `SET key val [EX ttl]` | `ttl_sec > 0` 时附加 `EX`；`<= 0` 写入永久 key |
| `del` | `DEL key` | key 不存在也返回成功 |

#### Hash

| 方法 | Redis 命令 | 说明 |
| --- | --- | --- |
| `hget` | `HGET key field` | 未命中返回 `SHIELD_REDIS_NIL` |
| `hset` | `HSET key field val` | 覆盖已有字段 |
| `hgetall` | `HGETALL key` | 返回 ARRAY，items 交替为 field/value STRING |

#### Sorted Set

| 方法 | Redis 命令 | 说明 |
| --- | --- | --- |
| `zadd` | `ZADD key score member` | member 已存在则更新 score |
| `zrange` | `ZRANGE key start stop` | 返回 ARRAY of STRING member |

#### Pipeline

| 方法 | 说明 |
| --- | --- |
| `pipeline` | 批量执行多条命令。`cmds` 为命令数组，`out_array` 为对应结果数组，`out_count` 返回实际结果数。每条命令的结果独立，某条失败不影响其他。 |

#### Raw Command

| 方法 | 说明 |
| --- | --- |
| `command` | 通用命令执行。`args[0]` 为命令名，后续为参数。覆盖所有 Redis 命令（`EVAL`、`XADD`、`INFO` 等）。 |

#### 内存管理

| 方法 | 说明 |
| --- | --- |
| `free_value` | 释放 `get`/`hget`/`hgetall`/`zrange`/`pipeline`/`command` 返回的 `shield_redis_value_v1*`。重复 free 安全（内部置空）。 |

### 获取 vtable

```cpp
#include "shield/plugin/redis.h"
#include "shield/plugin/plugin_host.hpp"

auto redis = shield::plugin::global_host()
                 .get_by_binding<shield_redis_v1>("redis.default");
```

### C++ 使用示例

```cpp
#include "shield/plugin/redis.h"
#include "shield/plugin/plugin_host.hpp"

auto redis = shield::plugin::global_host()
                 .get_by_binding<shield_redis_v1>("redis.default");

// Get an opaque handle for all subsequent calls.
char err_buf[256];
void* h = redis->connect(nullptr, err_buf, sizeof(err_buf));
if (!h) { /* handle error */ }

shield_error_v1 err{};

// SET session:1 "online" EX 60
redis->set(h, "session:1", "online", 60, &err);

// GET session:1
shield_redis_value_v1* val = nullptr;
if (redis->get(h, "session:1", &val, &err) == 0 && val) {
    if (val->type == SHIELD_REDIS_STRING) {
        // 使用 val->str (长度 val->str_len)
    }
    redis->free_value(val);
}

// HSET user:1 name alice
redis->hset(h, "user:1", "name", "alice", &err);

// HGETALL user:1
shield_redis_value_v1* user = nullptr;
if (redis->hgetall(h, "user:1", &user, &err) == 0 && user) {
    for (uint64_t i = 0; i + 1 < user->item_count; i += 2) {
        auto& field = user->items[i];
        auto& value = user->items[i + 1];
        // field.str, value.str
    }
    redis->free_value(user);
}

// ZADD rank:arena 1200 alice
redis->zadd(h, "rank:arena", 1200.0, "alice", &err);

// Raw command: EXPIRE session:1 300
shield_redis_arg_v1 args[] = {
    {"EXPIRE", 6},
    {"session:1", 9},
    {"300", 3},
};
shield_redis_value_v1* cmd_result = nullptr;
redis->command(h, args, 3, &cmd_result, &err);
if (cmd_result) redis->free_value(cmd_result);
```

### Pipeline 示例

```cpp
// MULTI command pipeline
shield_redis_arg_v1 set_args[] = {{"SET", 3}, {"k1", 2}, {"v1", 2}};
shield_redis_arg_v1 incr_args[] = {{"INCR", 4}, {"counter", 7}};
shield_redis_arg_v1 mget_args[] = {{"MGET", 4}, {"k1", 2}, {"k2", 2}};

shield_redis_command_v1 cmds[] = {
    {set_args, 3},
    {incr_args, 2},
    {mget_args, 3},
};

shield_redis_value_v1* results = nullptr;
uint64_t result_count = 0;
if (redis->pipeline(h, cmds, 3, &results, &result_count, &err) == 0) {
    for (uint64_t i = 0; i < result_count; ++i) {
        // results[i] 对应 cmds[i] 的返回值
    }
    redis->free_value(results);
}

// Release the handle when done (pool stays alive).
redis->disconnect(h);
```

## Lua API

`redis.driver` 通过 `register_lua` 暴露 `shield.redis(binding)`，返回 per-instance proxy。

### 基本用法

```lua
local redis = shield.redis("redis.default")

-- Key-Value
local ok, val = redis:get("session:1")
redis:set("session:1", "online", 60)  -- ttl 60 秒

-- Hash
redis:hset("user:1", "name", "alice")
local ok, user = redis:hgetall("user:1")
-- user = {name = "alice", email = "alice@example.com"}

-- Sorted Set
redis:zadd("rank:arena", 1200, "alice")
local ok, top = redis:zrange("rank:arena", 0, 9)
-- top = {"alice", "bob", ...}

-- Raw command
redis:command("EXPIRE", "session:1", "300")
redis:command("JSON.GET", "doc:1")

-- Pipeline
local ok, results = redis:pipeline({
    {"SET", "k1", "v1"},
    {"SET", "k2", "v2"},
    {"MGET", "k1", "k2"},
})
```

### Lua 返回值约定

| 方法 | 成功 | 失败 |
| --- | --- | --- |
| `get` | `true, value` 或 `true, nil`（未命中） | `false, {code=, message=}` |
| `set` | `true` | `false, {code=, message=}` |
| `del` | `true, count` | `false, {code=, message=}` |
| `hget` | `true, value` 或 `true, nil` | `false, {code=, message=}` |
| `hset` | `true` | `false, {code=, message=}` |
| `hgetall` | `true, {field=value, ...}` | `false, {code=, message=}` |
| `zadd` | `true` | `false, {code=, message=}` |
| `zrange` | `true, {member, ...}` | `false, {code=, message=}` |
| `command` | `true, result` | `false, {code=, message=}` |
| `pipeline` | `true, {result, ...}` | `false, {code=, message=}` |

### Typed Helper

Lua proxy 内置 typed helper，无需手写 `command("SET", ...)`：

```lua
-- 以下两种写法等价
redis:set("k", "v", 60)
redis:command("SET", "k", "v", "EX", "60")

-- 以下两种写法等价
local ok, val = redis:get("k")
local ok, val = redis:command("GET", "k")
```

helper 覆盖的方法：`get`、`set`、`del`、`hget`、`hset`、`hgetall`、`zadd`、`zrange`。

## 插件依赖

上层 Redis 插件通过 manifest 声明对 `shield.redis.v1` 的依赖。

### manifest 声明

```yaml
# cache.redis/manifest.yaml
requires:
  - name: redis
    interface: shield.redis.v1
    optional: false
```

```yaml
# leaderboard.redis/manifest.yaml
requires:
  - name: redis
    interface: shield.redis.v1
    optional: false
```

```yaml
# queue.redis/manifest.yaml
requires:
  - name: redis
    interface: shield.redis.v1
    optional: false
```

### 依赖注入

上层插件在 `create` 阶段通过 `host_api->dependency()` 获取 vtable：

```cpp
int cache_create(const shield_plugin_create_args_v1* args,
                 shield_plugin_instance_v1** out,
                 shield_error_v1* err) {
    // 获取 redis.driver 暴露的 shield.redis.v1
    const shield_redis_v1* redis = static_cast<const shield_redis_v1*>(
        args->host_api->dependency(args->ctx, "redis", SHIELD_REDIS_V1));

    if (!redis) {
        // redis.driver 未启动或未配置依赖
        err->code = "plugin.dependency.missing";
        err->message = "cache.redis requires shield.redis.v1";
        return -1;
    }

    // 后续使用 redis->get(...)、redis->hset(...) 等
    // ...
}
```

### 启动顺序

host 的拓扑排序保证 `redis.driver` 先于依赖它的插件启动：

1. `redis.main`（`redis.driver`）→ start → `state = started`
2. `cache.session`（`cache.redis`）→ `dependency("redis", "shield.redis.v1")` 返回有效 vtable → start
3. `leaderboard.global`（`leaderboard.redis`）→ 同上

## 能力覆盖

完整 Redis 能力通过以下入口覆盖：

| 场景 | 入口 | 说明 |
| --- | --- | --- |
| 普通命令 | `get`/`set`/`hset`/`zadd`/... | typed API，编译期类型安全 |
| 批量命令 | `pipeline(...)` | 减少网络往返 |
| Lua 脚本 | `command("EVAL", ...)` | 通过 raw command |
| Stream | `command("XADD"/"XREADGROUP"/...)` | 通过 raw command |
| 事务 | `pipeline({"MULTI"}, ..., {"EXEC"})` | 通过 pipeline |
| Pub/Sub | 建议独立接口 `shield.redis.pubsub.v1` | 避免阻塞命令池 |
| Cluster | 插件内部 redis++ cluster client | 配置 `mode: cluster` |

## 迁移路径

现有 `cache.redis`、`queue.redis`、`leaderboard.redis` 分阶段迁移。

### Phase 1：新增 redis.driver

- 新增 `redis.driver` 插件，不动现有插件。
- 现有插件继续自持 Redis 连接，独立运行。

### Phase 2：可选依赖

- 给 `cache.redis` 等插件增加可选依赖 `shield.redis.v1`。
- 有依赖时走 `redis.driver` 的 vtable；无依赖时保留旧内部连接（向后兼容）。

```yaml
# Phase 2: 可选依赖
requires:
  - name: redis
    interface: shield.redis.v1
    optional: true   # 可选，无依赖时走旧路径
```

### Phase 3：默认依赖

- Redis 系插件默认依赖 `redis.driver`，`optional: false`。
- 移除各业务 Redis 插件内部 redis++ 连接池代码。
- 配置 schema 移除 `host`/`port`/`password`/`pool_size` 等连接字段。

### 迁移前后对比

**Before** — 每个插件各自配 Redis 连接：

```yaml
plugins:
  instances:
    - id: cache.session
      package: cache.redis
      config:
        host: 127.0.0.1
        port: 6379
        pool_size: 8
    - id: leaderboard.global
      package: leaderboard.redis
      config:
        host: 127.0.0.1
        port: 6379
```

**After** — `redis.driver` 统一管理连接：

```yaml
plugins:
  instances:
    - id: redis.main
      package: redis.driver
      config:
        host: 127.0.0.1
        port: 6379
        pool_size: 16
    - id: cache.session
      package: cache.redis
      dependencies:
        redis: redis.main
    - id: leaderboard.global
      package: leaderboard.redis
      dependencies:
        redis: redis.main
```

## 部署

### 二进制位置

```
<shield-runtime>/
└── plugins/
    └── redis.driver/
        ├── manifest.yaml
        └── bin/
            ├── libshield_redis_driver.dll      # Windows
            ├── libshield_redis_driver.so       # Linux
            └── libshield_redis_driver.dylib    # macOS
```

### 连接池规划

`pool_size` 直接控制 redis-plus-plus 的 `ConnectionPoolOptions.size`。所有依赖 `redis.driver` 的上层插件共享同一个连接池。

| 场景 | 推荐 pool_size |
| --- | --- |
| 单 worker，低 QPS | 4-8 |
| 多上层插件共享（cache + leaderboard） | 16-32 |
| 高并发 + pipeline 密集 | 32-64 |

连接池满时新请求会阻塞等待空闲连接，直到 `command_timeout_ms`。

### Redis 版本要求

- `single` 模式：Redis 2.0+（建议 6.0+ 以获得 ACL 和 TLS）
- `sentinel` 模式：Redis 2.8+（Sentinel v1）
- `cluster` 模式：Redis 3.0+

### 线程安全

| 入口 | 线程安全 |
| --- | --- |
| typed 方法（`get`/`set`/...） | 线程安全（走连接池） |
| `pipeline` | 线程安全（单连接原子执行，池化分配） |
| `command` | 线程安全 |
| `free_value` | 不可并发释放同一个 value |

### TLS 配置

启用 TLS 时，redis++ 通过 hiredis 的 TLS 支持建立安全连接：

```yaml
config:
  tls:
    enabled: true
    cert_path: "/etc/redis/tls/client.crt"
    key_path: "/etc/redis/tls/client.key"
    ca_cert_path: "/etc/redis/tls/ca.crt"
```

需要 hiredis 编译时启用 `-DENABLE_SSL=ON`，且 Redis 服务端配置 TLS 端口。

## 相关链接

- [插件系统](/plugin-system)
- [插件参考索引](/plugins/)
- [cache.redis](/plugins/cache-redis)
- [queue.redis](/plugins/queue-redis)
- [leaderboard.redis](/plugins/leaderboard-redis)
- [Redis 官方文档](https://redis.io/docs/)
- [redis-plus-plus](https://github.com/sewenew/redis-plus-plus)
