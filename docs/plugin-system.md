# Shield 通用插件系统

## 设计目标

| 目标 | 说明 |
|------|------|
| **语言无关** | 任何能编译为 C ABI 共享库的语言都能写插件（C/C++/Rust/Go/Zig） |
| **稳定 ABI** | 版本化接口，向前兼容，不破坏已有插件 |
| **类型安全** | 每种插件类型有独立的 vtable，避免 void* 泛型 |
| **运行时加载** | 启动时发现、加载、初始化；运行时可查询能力 |
| **Lua 可访问** | 插件能力可通过 `shield.plugin.*` 暴露给 Lua |
| **零依赖** | 插件不依赖 Shield 内部头文件，只依赖 `plugin.h` |

## 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│                     Shield Core                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
│  │ PluginManager│  │ PluginLoader │  │ PluginRegistry   │  │
│  │  - discover  │  │  - load DLL  │  │  - name → plugin │  │
│  │  - init all  │  │  - resolve   │  │  - type → list   │  │
│  │  - shutdown  │  │    symbols   │  │  - capability idx│  │
│  └──────┬───────┘  └──────┬───────┘  └────────┬─────────┘  │
│         └─────────────────┼────────────────────┘            │
│                    ┌──────▼──────┐                          │
│                    │ plugin.h    │  (C ABI, stable)          │
│                    └──────┬──────┘                          │
└───────────────────────────┼─────────────────────────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
┌───────▼───────┐  ┌────────▼────────┐  ┌───────▼───────┐
│ shield_db_    │  │ shield_auth_    │  │ shield_game_  │
│ mysql.dll     │  │ jwt.dll         │  │ combat.dll    │
│ (DATABASE)    │  │ (AUTH)          │  │ (GAME)        │
│ vtable →      │  │ vtable →        │  │ vtable →      │
│  db_plugin_t  │  │  auth_plugin_t  │  │  game_plugin_t│
└───────────────┘  └─────────────────┘  └───────────────┘
```

## 插件如何感知 Shield 上下文

插件通过 **Host Context** 机制访问 Shield 运行时，无需链接 Shield 库：

```c
// init 时接收 host 和 host_api
int my_plugin_init(shield_host_t host,
                   const shield_host_api* host_api,
                   const shield_plugin_config* config,
                   char* err_buf, int err_buf_size) {
    // 写日志
    host_api->log(SHIELD_LOG_INFO, "my_plugin", "initialized");

    // 读配置
    const char* host_str = host_api->get_config(host, "database.host");

    // 查找其他插件
    const shield_plugin* db = host_api->find_plugin(host, "shield_db_mysql");
    if (db) {
        // 获取其他插件的 vtable
        const shield_db_plugin* db_api = db->vtable;
        auto* conn = db_api->connect(...);
    }

    // 报告错误
    host_api->report_error(host, "my_plugin", "connection_lost", "DB unreachable");

    return 0;
}
```

| Host API 方法 | 功能 |
|--------------|------|
| `log(level, name, msg)` | 写入 Shield 日志系统 |
| `get_config(host, key)` | 读取配置值 |
| `find_plugin(host, name)` | 查找已加载插件 |
| `get_plugin_vtable(host, name)` | 获取其他插件的 vtable |
| `register_shutdown_hook(host, fn, data)` | 注册关闭钩子 |
| `report_error(host, plugin, code, msg)` | 报告插件错误 |

## 目录结构

```
include/shield/plugin/           ← 插件接口（独立，不耦合任何模块）
├── plugin.h                     ← 通用插件基接口 + Host Context
│
│  基础设施插件（共享依赖）
├── db_plugin.h                  ← 数据库插件接口
├── redis_plugin.h               ← Redis 插件接口（CACHE/QUEUE/LEADERBOARD 共享）
│
│  业务插件
├── cache_plugin.h               ← 缓存插件（依赖 Redis）
├── queue_plugin.h               ← 队列插件（依赖 Redis）
├── leaderboard_plugin.h         ← 排行榜插件（内存 Skip List / Redis / SQLite / JSON）
├── auth_plugin.h                ← 认证插件（JWT/OAuth/Steam/微信）
├── matchmaking_plugin.h         ← 匹配插件（ELO/MMR/技能匹配）
├── metric_plugin.h              ← 指标导出（Prometheus/StatsD/Datadog）
└── health_plugin.h              ← 健康检查（HTTP/TCP 端点）

plugins/                         ← 插件实现（每个子目录是一个 DLL）
├── mysql/shield_db_mysql.cpp
├── postgresql/shield_db_pgsql.cpp
├── sqlite/shield_db_sqlite.cpp
└── redis/shield_redis.cpp       ← Redis 基础设施插件
```

## Redis ZSET 多字段编码

Redis ZSET 的 score 是 `double`（53 位有效整数），可以打包多个字段：

```c
// 编码：score(20位) + level(10位) + time_rank(20位)
double fields[] = {1500, 45, 12345};
int bits[] = {20, 10, 20};
double composite = redis->encode_composite(fields, bits, 3);
redis->zadd("leaderboard", composite, "player:123");

// 查询时自动按 composite 排序 = score DESC + level DESC + time ASC
// 解码获取原始字段
double decoded[3];
redis->decode_composite(composite, bits, 3, decoded);
// decoded[0] = 1500, decoded[1] = 45, decoded[2] = 12345
```

**排序方向控制：**
- DESC 字段：直接用原值
- ASC 字段：用 `max_value - value` 反转（如时间戳：`max_time - timestamp`）

这样 Redis ZSET 也能支持多字段排序，和内存 Skip List 一样灵活。

## 插件依赖关系

```
shield_redis (基础设施)
├── shield_cache    (依赖 Redis 做存储)
├── shield_queue    (依赖 Redis 做 pub/sub)

shield_leaderboard  (独立，支持多种后端)
├── 后端: memory (Skip List) / redis (ZSET) / sqlite / json
├── 多字段排序: score DESC + level DESC + time ASC
├── 小游戏: 内存 Skip List + JSON 持久化，零外部依赖
└── 大型游戏: Redis ZSET 或 SQLite

shield_db_mysql     (独立)
shield_auth_jwt     (独立)
shield_matchmaking  (独立)
shield_metric       (独立)
shield_health       (独立)
```

## 排行榜后端选择

| 后端 | 插入 | 排名查询 | Top N | 多字段 | 持久化 | 适合场景 |
|------|------|---------|-------|--------|--------|---------|
| **memory (Skip List)** | O(log N) | O(log N) | O(log N+M) | ✅ | 可选 JSON | 小型单机游戏 |
| **Redis ZSET** | O(log N) | O(log N) | O(log N+M) | ✅ 编码 | Redis 持久化 | 大型分布式 |
| **SQLite** | O(log N) | O(log N) | O(log N+M) | ✅ | ✅ 原生 | 需要持久化查询 |
| **JSON 文件** | O(N log N) | O(N) | O(M) | ✅ | ✅ 文件 | 开发/测试 |

**通过 Host Context 获取依赖：**
```c
// CACHE 插件 init 时获取 Redis 插件
int cache_init(shield_host_t host, const shield_host_api* api, ...) {
    const shield_plugin* redis_p = api->find_plugin(host, "shield_redis");
    const shield_redis_plugin* redis = redis_p->vtable;
    // 用 redis->get/set 做缓存操作
}
```

**设计原则：插件接口是独立契约，`shield_data`、`shield_lua`、`shield_net` 都是消费者。**

## 核心接口 (plugin.h)

```c
// 每个插件类型有独立的 vtable（类型安全，不是 void*）
enum shield_plugin_type {
    DATABASE    = 0x01,   // vtable → shield_db_plugin
    TRANSPORT   = 0x02,   // vtable → shield_transport_plugin (Phase 2)
    AUTH        = 0x03,   // vtable → shield_auth_plugin
    CACHE       = 0x04,   // vtable → shield_cache_plugin (依赖 Redis)
    QUEUE       = 0x05,   // vtable → shield_queue_plugin (依赖 Redis)
    STORAGE     = 0x06,   // vtable → shield_storage_plugin (Phase 2)
    METRIC      = 0x07,   // vtable → shield_metric_plugin
    HEALTH      = 0x08,   // vtable → shield_health_plugin
    LOG         = 0x09,   // vtable → shield_log_plugin (Phase 2)
    GATEWAY     = 0x0A,   // vtable → shield_gateway_plugin (Phase 2)
    LEADERBOARD = 0x0B,   // vtable → shield_leaderboard_plugin (依赖 Redis)
    MATCHMAKING = 0x0C,   // vtable → shield_matchmaking_plugin
    REDIS       = 0x0D,   // vtable → shield_redis_plugin (基础设施)
    GAME        = 0x10,   // vtable → 用户自定义
    USER        = 0xFF,   // vtable → 用户自定义
};

struct shield_plugin_t {
    uint32_t abi_version;       // 必须 = SHIELD_PLUGIN_ABI_VERSION
    shield_plugin_type type;
    const char* name;           // "mysql", "jwt-auth", "combat"
    const char* version;        // "1.0.0"
    const char* description;

    // 生命周期
    int  (*init)(const shield_plugin_config* config, char* err, int err_len);
    void (*shutdown)(void);

    // 能力声明
    int  (*capability_count)(void);
    const shield_plugin_capability* (*get_capability)(int index);

    // 类型特化 vtable（由 type 决定强转类型）
    const void* vtable;
};

// 插件入口点（每个 DLL 必须导出）
SHIELD_PLUGIN_EXPORT
const shield_plugin_t* shield_plugin_api(void);
```

## 插件类型与 vtable

### DATABASE (已有)

复用 `db_plugin.h`，vtable 为 `shield_db_plugin_t`。

### AUTH (Phase 2)

```c
struct shield_auth_plugin_t {
    int (*authenticate)(const char* token, shield_auth_result*);
    int (*authorize)(const char* user_id, const char* resource, const char* action);
    void (*free_result)(shield_auth_result*);
};
```

### CACHE (Phase 2)

```c
struct shield_cache_plugin_t {
    shield_cache_conn* (*connect)(const shield_cache_config*, char*, int);
    void (*disconnect)(shield_cache_conn*);
    int (*get)(shield_cache_conn*, const char*, shield_cache_value*);
    int (*set)(shield_cache_conn*, const char*, const char*, int ttl);
    int (*del)(shield_cache_conn*, const char*);
    void (*free_value)(shield_cache_value*);
};
```

### GAME (Phase 2)

```c
struct shield_game_plugin_t {
    int (*on_init)(void);
    int (*on_tick)(int64_t now_ms);
    void (*on_shutdown)(void);
    const char* (*handle_rpc)(const char* method, const char* params_json);
};
```

## YAML 配置

```yaml
plugins:
  directory: "./plugins"
  enabled:
    - shield_db_mysql
  disabled:
    - shield_db_sqlite
  config:
    shield_db_mysql:
      host: localhost
      port: 3306
```

## Lua API

```lua
local plugins = shield.plugin.list()
local dbs = shield.plugin.by_type("database")
local ok = shield.plugin.loaded("shield_db_mysql")
local caps = shield.plugin.capabilities("shield_db_mysql")
```

## 实现分阶段

| Phase | 内容 | 状态 |
|-------|------|------|
| **P0** | `plugin.h` C ABI + `PluginManager` + 现有 `db_plugin` 适配 | 本次实现 |
| **P1** | YAML 配置加载 + `shield.plugin.*` Lua API | 本次实现 |
| **P2** | AUTH/CACHE/GAME 插件类型 | Phase 2+ |
| **P3** | 插件热加载 + 依赖管理 + 沙箱 | Phase 3+ |

## 与现有代码的关系

| 现有组件 | 改造方式 |
|---------|---------|
| `db_plugin.h` | 保留，作为 `DATABASE` 类型的 vtable |
| `PluginDatabaseConnection` | 保留，由 `PluginManager` 创建 |
| `dynamic_library.hpp` | 泛化为通用插件加载器 |
| `shield_data` | 改为从 `PluginManager` 获取数据库插件 |
