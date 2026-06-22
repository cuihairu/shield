# cache.redis

> 基于 Redis 的 key-value、hash、TTL 与计数器缓存，实现 `shield.cache.v1` 接口。

## 包信息

- **包 ID**: `cache.redis`
- **接口**: [`shield.cache.v1`](/plugin-system#interface-model)
- **Capabilities**: `kv`、`hash`、`ttl`、`counter`
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_PLUGIN_CACHE_REDIS`
- **源码**: `plugins/cache.redis/`
- **依赖**: redis-plus-plus（redis++）、hiredis（通过 vcpkg）

## 构建启用

`cache.redis` 默认跟随主工程构建，可通过 CMake 选项单独关闭：

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_CACHE_REDIS=ON
```

构建产物输出到 `<build>/plugins/cache.redis/`，包含 `manifest.yaml` 和 `bin/` 下的共享库。运行时由 host 扫描 `plugins.directory` 自动发现。

## 配置 Schema

配置通过 `plugins.instances[].config` 注入，字段直接映射到 `shield_cache_config`。

| 字段 | 类型 | 必填 | 默认值 | 说明 |
| --- | --- | --- | --- | --- |
| `host` | string | 是 | `127.0.0.1` | Redis 主机地址 |
| `port` | integer | 否 | `6379` | Redis 端口，范围 1-65535 |
| `password` | string | 否 | - | 鉴权密码，`secret: true` 在日志中脱敏 |
| `db` | integer | 否 | `0` | Redis DB 索引，范围 0-15 |
| `pool_size` | integer | 否 | `8` | 连接池大小，范围 1-256 |
| `connect_timeout_ms` | integer | 否 | `5000` | 建连超时，范围 100-60000 毫秒 |
| `command_timeout_ms` | integer | 否 | `5000` | 单条命令超时，范围 100-60000 毫秒 |

完整 `app.yaml` 示例：

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: cache.session
      package: cache.redis
      required: true
      config:
        host: "127.0.0.1"
        port: 6379
        password: "s3cret"
        db: 0
        pool_size: 16
        connect_timeout_ms: 3000
        command_timeout_ms: 2000
    - id: cache.ranking
      package: cache.redis
      required: false
      config:
        host: "10.0.0.20"
        port: 6380
        db: 2
        pool_size: 32
  bindings:
    cache.default: cache.session
    cache.ranking: cache.ranking
```

## 接口契约

源文件：`include/shield/plugin/cache.h`。每个实例通过 `connect()` 拿到一个独立的 `shield_cache_conn*`，背后是一个独立的 redis-plus-plus `Redis` 对象（带连接池）。v1 不存在公共 redis 基础设施插件，多个 cache 实例即使指向同一个 Redis，也只是通过 Redis 自身共享数据，host 不做胶水。

### 连接管理

```c
struct shield_cache_conn* (*connect)(const struct shield_cache_config* cfg,
                                     char* err_buf, int err_buf_size);
void (*disconnect)(struct shield_cache_conn* conn);
int  (*ping)(struct shield_cache_conn* conn);
```

- `connect` — 建立 redis-plus-plus 连接池，立即 `PING` 校验后返回。失败时写入 `err_buf` 并返回 `nullptr`。
- `disconnect` — 释放连接对象，连接池随之析构。
- `ping` — 执行 Redis `PING`，成功返回 `1`，异常返回 `0`。用于健康检查。

### Key-Value 操作

```c
int (*get)(struct shield_cache_conn* conn, const char* key,
           struct shield_cache_value* out);
int (*set)(struct shield_cache_conn* conn, const char* key,
           const char* value, int ttl_seconds);
int (*del)(struct shield_cache_conn* conn, const char* key);
int (*exists)(struct shield_cache_conn* conn, const char* key);
```

- `get` — Redis `GET`。命中时 `out->found = 1` 并填充 `data`/`data_len`；未命中 `found = 0`。
- `set` — Redis `SET`。`ttl_seconds > 0` 时附加 `EX` 过期；`ttl_seconds <= 0` 表示永久。
- `del` — Redis `DEL`，删除不存在的 key 也返回 `0`。
- `exists` — Redis `EXISTS`，存在返回 `1`，不存在或异常返回 `0`。

### 计数器

```c
int (*incr)(struct shield_cache_conn* conn, const char* key, int64_t* out);
int (*incr_by)(struct shield_cache_conn* conn, const char* key,
               int64_t amount, int64_t* out);
```

- `incr` — Redis `INCR`，原子自增 1，结果写入 `*out`。对不存在的 key 会初始化为 0 再自增。
- `incr_by` — Redis `INCRBY`，按 `amount` 原子增减（`amount` 可为负数）。

### Hash 操作

```c
int (*hget)(struct shield_cache_conn* conn, const char* key,
            const char* field, struct shield_cache_value* out);
int (*hset)(struct shield_cache_conn* conn, const char* key,
            const char* field, const char* value);
int (*hdel)(struct shield_cache_conn* conn, const char* key,
            const char* field);
```

- `hget` — Redis `HGET`，命中语义同 `get`。
- `hset` — Redis `HSET`，覆盖已有字段。
- `hdel` — Redis `HDEL`，字段不存在也返回 `0`。

### 内存管理

```c
void (*free_value)(struct shield_cache_value* value);
```

`get` / `hget` 返回的 `data` 由插件 `malloc` 分配，调用方必须用 `free_value` 释放，否则内存泄漏。释放后 `data` 置空、`data_len` 清零。

### shield_cache_value 语义

```c
struct shield_cache_value {
    int found;
    const char* data;
    int data_len;
    int64_t ttl_remaining_ms;  // -1 = no TTL
};
```

- `found` — 0 表示未命中，1 表示命中。
- `data` — 命中时指向 `malloc` 出的字节缓冲，可能包含任意二进制（按字符串读 `data_len` 字节）。
- `ttl_remaining_ms` — 当前实现固定为 `-1`（不查询 TTL），保留字段以便未来扩展。

## 使用示例

### C++（通过 binding）

```cpp
#include "shield/plugin/cache.h"
#include "shield/plugin/plugin_host.hpp"

auto cache = shield::plugin::global_host()
                 .get_by_binding<shield_cache_v1>("cache.default");

shield_cache_config cfg{};
cfg.host = "127.0.0.1";
cfg.port = 6379;
cfg.pool_size = 8;
cfg.connect_timeout_ms = 5000;
cfg.command_timeout_ms = 5000;

char err_buf[256];
shield_cache_conn* conn = cache->connect(&cfg, err_buf, sizeof(err_buf));
if (!conn) {
    // err_buf 包含异常信息
    return;
}

cache->set(conn, "player:1234", R"({"level":7})", 3600);

shield_cache_value v{};
if (cache->get(conn, "player:1234", &v) == 0 && v.found) {
    // 使用 v.data（长度 v.data_len）
    cache->free_value(&v);
}

int64_t new_rank = 0;
cache->incr_by(conn, "player:1234:rank", 5, &new_rank);

cache->disconnect(conn);
```

### Lua

`cache.redis` 通过 `register_lua` 暴露 callable table 形式的多实例 proxy：

```lua
local cache = shield.cache.redis("cache.session")
local ok, err = cache:set("player:1234", json.encode({level = 7}), 3600)
if not ok then
  -- 处理 err.message
end

local ok, value_or_err = cache:get("player:1234")
if ok and value_or_err then
  local data = json.decode(value_or_err)
end
```

可用方法包括 `get`、`set`、`del`、`exists`、`incr`、`incr_by`、`hget`、`hset`、`hdel`。Lua 方法通常返回 `ok, result_or_error`；`get` / `hget` 未命中时返回 `true, nil`。

## 特殊语义

### 过期策略

`set` 的 `ttl_seconds` 参数直接映射到 Redis `EX` 选项。过期由 Redis 服务端裁决，插件不做客户端 TTL。`ttl_seconds <= 0` 写入的是永久 key，需要业务自行清理或依赖 Redis eviction policy。`get` 返回的 `ttl_remaining_ms` 当前固定为 `-1`，不调用 `PTTL`，避免额外往返。

### 原子性

`incr` 和 `incr_by` 直接对应 Redis 单命令，原子性由 Redis 单线程模型保证。插件没有暴露 `MULTI`/`EXEC` 或 Lua 脚本入口；需要复合原子操作时，业务应在服务端使用 Lua 脚本，或通过 `set` + `incr` 的 CAS 模式（配合 `WATCH`，但 v1 接口未暴露 `WATCH`）。

### Pipeline 支持

v1 接口未提供显式 pipeline，但 redis-plus-plus 内部连接池支持并发请求。多线程从同一个 `shield_cache_conn*` 调用是安全的（连接池会分发到不同底层连接）。批量写入建议业务层异步化或拆分实例。

### 序列化约定

cache 层对 `value` 完全透明，按字节存取。推荐使用 JSON 或 MessagePack 等显式格式，避免 Lua table / C++ 对象的隐式序列化造成跨语言读写不一致。`data_len` 字段用于支持二进制负载，调用方必须按长度读取而非按 `\0` 截断。

## 错误处理

所有方法返回 `int`：`0` 成功，`-1` 失败。失败原因不会写入 `shield_error_v1`，而是：

| 方法 | 失败时的副作用 |
| --- | --- |
| `connect` | 返回 `nullptr`，`err_buf` 写入异常 `what()` |
| `get` / `hget` | 返回 `-1`，`out->found` 强制为 `0`、`data = nullptr` |
| `set` / `del` / `exists` | 返回 `-1` 或 `0`，调用方无法区分“key 不存在”和“网络异常” |
| `incr` / `incr_by` | 返回 `-1`，`*out` 未定义 |

建议在生产环境对关键写操作做重试包装，并通过 `ping` 做前置健康检查。Redis 异常（连接断开、超时、鉴权失败）都会被 `catch(...)` 吞掉，仅以返回码暴露。

## 部署

### 二进制位置

```
<shield-runtime>/
└── plugins/
    └── cache.redis/
        ├── manifest.yaml
        └── bin/
            ├── libshield_cache_redis.dll      # Windows
            ├── libshield_cache_redis.so       # Linux
            └── libshield_cache_redis.dylib    # macOS
```

### 运行时依赖

- Redis 服务端 >= 4.0（`SET ... EX`、`INCRBY` 自远古版本即支持，但建议 6+ 以获得 TLS 和 ACL）
- hiredis（redis-plus-plus 的底层 C 客户端，vcpkg 已固化）
- TLS / ACL 等高级特性当前未通过 config schema 暴露，需要时请扩展 `extra_json`

### 连接池配置

`pool_size` 直接控制 redis-plus-plus 的 `ConnectionPoolOptions.size`。建议：

| 场景 | 推荐 pool_size |
| --- | --- |
| 单 worker 进程，低 QPS | 4-8 |
| 高并发读取（缓存层） | 16-32 |
| 写密集 + 多实例共享 Redis | 8-16，并水平扩展实例数 |

连接池满时新请求会阻塞等待空闲连接，直到 `command_timeout_ms`。监控 `redis_connected_clients` 和客户端队列深度可以早期发现池太小的问题。

## 相关链接

- [插件系统](/plugin-system)
- [插件参考索引](/plugins/)
- [Redis 官方文档](https://redis.io/docs/)
- [redis-plus-plus](https://github.com/sewenew/redis-plus-plus)
