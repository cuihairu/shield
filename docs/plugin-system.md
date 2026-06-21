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

## 核心接口 (plugin.h)

```c
// 每个插件类型有独立的 vtable（类型安全，不是 void*）
enum shield_plugin_type {
    DATABASE  = 0x01,   // vtable → shield_db_plugin_t
    TRANSPORT = 0x02,   // vtable → shield_transport_plugin_t
    AUTH      = 0x03,   // vtable → shield_auth_plugin_t
    CACHE     = 0x04,   // vtable → shield_cache_plugin_t
    GAME      = 0x10,   // vtable → 用户自定义
    USER      = 0xFF,   // vtable → 用户自定义
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
