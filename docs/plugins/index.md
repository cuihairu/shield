# 插件参考

Shield 通过插件系统 v1 提供后端能力。每个插件是一个独立的 shared library，附带 `plugin.json` manifest 描述它提供什么接口、需要什么配置。本节为每个官方插件提供单独的参考页：包含 manifest 字段、配置 schema、ABI 接口、Lua API（如果有）和使用示例。

## 官方插件清单

### 数据库

| 包 ID | 接口 | 说明 |
|-------|------|------|
| [database.sqlite](/plugins/database-sqlite) | `shield.database.v1` | 嵌入式 SQL 数据库，零部署 |
| [database.mysql](/plugins/database-mysql) | `shield.database.v1` | MySQL X DevAPI |
| [database.postgresql](/plugins/database-postgresql) | `shield.database.v1` | PostgreSQL (libpq) |

### 缓存与队列

| 包 ID | 接口 | 说明 |
|-------|------|------|
| [cache.redis](/plugins/cache-redis) | `shield.cache.v1` | Redis key-value、hash、TTL、counter |
| [queue.redis](/plugins/queue-redis) | `shield.queue.v1` | Redis pub/sub |
| [leaderboard.redis](/plugins/leaderboard-redis) | `shield.leaderboard.v1` | Redis ZSET 排行榜，支持复合评分 |

### 平台服务

| 包 ID | 接口 | 说明 |
|-------|------|------|
| [auth.jwt](/plugins/auth-jwt) | `shield.auth.v1` | HS256 JWT 认证 |
| [metrics.prometheus](/plugins/metrics-prometheus) | `shield.metrics.v1` | Prometheus 文本格式指标导出 |
| [health.http](/plugins/health-http) | `shield.health.v1` | Kubernetes 兼容的健康检查端点 |
| [matchmaking.elo](/plugins/matchmaking-elo) | `shield.matchmaking.v1` | ELO 匹配算法 |

## 加载机制

host 启动时按以下顺序处理插件：

1. **scan** — 读取 `<plugins_dir>/<package_id>/plugin.json`
2. **catalog** — 检查包 ID 唯一性、平台库路径存在
3. **plan + resolve** — 解析实例间依赖、拓扑排序
4. **load** — `dlopen` + ABI 版本/大小/包 ID 校验
5. **create** — 调用 `shield_plugin_get_v1()->create()`，传入实例 config
6. **start** — 按拓扑顺序调用 `instance->start()`
7. **lua_init** — 初始化 Lua 运行时
8. **lua_register** — 按相同顺序调用每个实例的 `register_lua(L)`

完整 pipeline 见 [插件系统](/plugin-system#bootstrap-pipeline)。

## 实例配置

在 Shield 主配置的 `plugins` 子树中声明插件实例。当前仓库默认使用 `config/app.yaml`；该子树进入插件系统后按 JSON-compatible 值模型处理：

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: db.main
      package: database.sqlite
      required: true
      config:
        database: "game.db"
        query_timeout_ms: 5000
    - id: cache.session
      package: cache.redis
      required: true
      config:
        host: "127.0.0.1"
        port: 6379
        pool_size: 8
  bindings:
    database.default: db.main
    cache.default: cache.session
```

业务代码通过 binding 名（而非实例 ID）访问，host 会解析到具体的 vtable：

```cpp
// C++
auto db = shield::plugin::global_host().get_by_binding<shield_database_v1>(
    "database.default");
```

```lua
-- Lua（v1 ABI Lua 自治模式下）
local db = shield.database.sqlite("db.main")
```

当前 binding 配置格式保持最小化：`logical_name: instance_id`。C++ 侧调用 `get_by_binding<T>()` 时由 `T::interface_name` 决定要获取的 ABI interface；Lua 侧插件 API 通常直接使用实例 ID 或插件自己定义的默认实例策略。

## 第三方插件开发指南

Shield 的插件**不需要**放在 Shield 源码树里。第三方插件是独立的 C++ 项目，只需引用 SDK 头文件并产出 shared library。

### 最小插件模板

项目结构：

```
my-shield-plugin/
├── CMakeLists.txt
├── plugin.json
├── src/
│   └── my_plugin.cpp
└── README.md
```

### plugin.json

```json
{
  "schema_version": 1,
  "id": "database.my-engine",
  "name": "My Engine",
  "version": "1.0.0",
  "kind": "database",
  "description": "My custom database engine",
  "documentation": {
    "url": "https://my-domain.example/docs/shield-plugin",
    "description": "Online reference for the my-engine Shield plugin"
  },
  "entry": "shield_plugin_get_v1",
  "library": {
    "windows": "bin/shield_my_engine.dll",
    "linux": "bin/libshield_my_engine.so",
    "macos": "bin/libshield_my_engine.dylib"
  },
  "provides": [
    { "interface": "shield.database.v1", "capabilities": ["sql"] }
  ],
  "requires": [],
  "config_schema": {
    "type": "object",
    "properties": {
      "connection_string": { "type": "string" }
    },
    "required": ["connection_string"]
  }
}
```

`documentation` 字段可选但**强烈推荐**：host 通过 `PluginHost::list_packages()` 把它暴露给 dashboard、`shield.plugin.list()` Lua API、`--check-config` 诊断输出，用户能直接点击跳转到你的在线文档。

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.30)
project(my_shield_plugin LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)

# Shield SDK headers - either installed via vcpkg or vendored.
# Only the shield/plugin/*.h headers are needed.
find_package(shield-plugin-sdk CONFIG REQUIRED)  # future
# 或简单粗暴:
# include_directories(${SHIELD_SDK_INCLUDE_DIR})

add_library(my_shield_plugin MODULE src/my_plugin.cpp)

target_include_directories(my_shield_plugin PRIVATE
    ${SHIELD_SDK_INCLUDE_DIR}
)

# 链接你自己的引擎库
target_link_libraries(my_shield_plugin PRIVATE my_engine_lib)

# Module library: 不要加 "lib" 前缀 on Windows, 不要 STATIC
set_target_properties(my_shield_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_my_engine"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/database.my-engine/bin"
)

# 部署 plugin.json
file(COPY plugin.json
     DESTINATION "${CMAKE_BINARY_DIR}/plugins/database.my-engine")
```

### 最小 C++ 实现

```cpp
// src/my_plugin.cpp
#include "shield/plugin/abi.h"
#include "shield/plugin/database.h"
#include "shield/plugin/host_api.h"

#include <cstring>
#include <string>

namespace {

struct my_conn {
    // 你的连接状态
    std::string uri;
};

// shield_database_v1 vtable — 实现所有方法
const shield_database_v1& my_vtable() {
    static const shield_database_v1 v = {
        sizeof(shield_database_v1),
        "my-engine",
        "1.0.0",
        // connect
        [](const shield_db_connect_args* args, char* err_buf, int err_buf_size) -> shield_db_conn* {
            // 解析 args->database 或 args->host 等
            return new my_conn{ /* ... */ };
        },
        // disconnect
        [](shield_db_conn* c) { delete static_cast<my_conn*>(c); },
        // ping
        [](shield_db_conn*) -> int { return 1; },
        // query
        [](shield_db_conn*, const char* sql, const char* const* params,
           int n_params, shield_db_result* out) -> int {
            // 执行查询，填充 out
            out->success = 1;
            return 0;
        },
        // execute / begin / commit / rollback / free_result ...
        // ...
    };
    return v;
}

struct my_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
};

int my_create(const shield_plugin_create_args_v1* args,
              shield_plugin_instance_v1** out,
              shield_error_v1* err) {
    (void)err;
    auto* inst = new my_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1*,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::string(iface) == SHIELD_DATABASE_INTERFACE)
            return &my_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) { return 0; };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        delete reinterpret_cast<my_instance*>(self);
    };
    // 即使没有 Lua 绑定，register_lua 也必须显式设置
    inst->shell.register_lua = [](shield_plugin_instance_v1*, struct lua_State*,
                                  shield_error_v1*) { return 0; };
    *out = &inst->shell;
    return 0;
}

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT
const shield_plugin_abi_v1* shield_plugin_get_v1(void) {
    static const shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "database.my-engine",   // 必须与 plugin.json "id" 完全一致
        "1.0.0",
        my_create,
    };
    return &abi;
}
```

### 部署

构建产物部署到 Shield 进程的工作目录：

```
<shield-runtime>/
└── plugins/
    └── database.my-engine/
        ├── plugin.json
        └── bin/
            └── libshield_my_engine.dll  (或 .so / .dylib)
```

host 启动时 `PluginHost::scan()` 自动发现并加载。

### ABI 稳定性

- `SHIELD_PLUGIN_ABI_VERSION = 1`
- host 通过 `abi_version`、`struct_size`、`package_id` 三重校验
- 只允许在 struct 末尾追加字段（host 用 `struct_size` 检测支持范围）
- 禁止重新排序、删除、改变现有字段含义

详细 ABI 契约见 [插件系统 - Binary ABI](/plugin-system#binary-abi)。

### SDK 分发路线图

目前 Shield 还没有提供独立的 SDK 包。计划中的分发方式：

- **vcpkg**：`shield-plugin-sdk` 端口，只安装 `include/shield/plugin/*.h`
- **模板仓库**：`shield-plugin-template`（GitHub template repository），含完整最小示例
- **CMake package config**：`find_package(shield-plugin-sdk CONFIG REQUIRED)`

在这些落地之前，第三方插件可以通过以下方式引用头文件：

```cmake
# 方式 1: git submodule
add_subdirectory(shield)
target_link_libraries(my_plugin PRIVATE shield_plugin)

# 方式 2: 仅复制头文件
set(SHIELD_SDK_INCLUDE_DIR "/path/to/shield/include")
target_include_directories(my_plugin PRIVATE ${SHIELD_SDK_INCLUDE_DIR})

# 方式 3: vcpkg overlay
# 编写 vcpkg port 只装头文件
```

## 设计原则

- **Metadata-first**：所有信息（接口、依赖、配置、文档 URL）声明在 `plugin.json`
- **C ABI 稳定**：跨编译器、跨版本兼容
- **接口名而非类型枚举**：`shield.database.v1` 字符串而非 `PLUGIN_TYPE_DATABASE`
- **依赖注入**：插件只能访问 manifest 中 `requires[]` 声明的依赖
- **Lua 自治**：插件目录自带 `lua/` 业务封装；通过 `register_lua` 自主注册 `shield.<namespace>` API
- **声明式文档**：`documentation.url` 让 dashboard 直接链接到在线文档

## 相关文档

- [插件系统](/plugin-system) — 完整设计、ABI 契约、bootstrap pipeline
- [Lua API 契约](/lua-api) — 业务 Lua 调用约定
- [目标目录结构](/directory-structure) — `plugins/` 在仓库中的位置
