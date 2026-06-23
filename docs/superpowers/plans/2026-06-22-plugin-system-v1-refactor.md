# Shield 插件系统 v1 重构 — 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **现行实现说明：** 当前运行时与正式文档已收敛到单一 manifest 来源，只支持 `manifest.yaml`。本文其余内容保留当时计划表述；下文若仍出现旧 manifest 文件名或旧表述，应按历史语境理解，不再代表当前契约。


**Goal:** 按 `docs/plugin-system.md` 完全重构 Shield 插件系统：新 metadata-first JSON manifest + catalog/pipeline + 稳定 C ABI（`shield_plugin_get_v1`）+ interface name 类型系统 + instance/binding 依赖注入 + 结构化错误，并迁移现有 8 个插件、改造 DatabasePool、实现 Lua introspection。

**Architecture:** 纵切端到端 + TDD。先建 ABI 头文件族与 `src/plugin/` 核心（manifest 解析 / schema 校验 / PluginHost pipeline），再打通 sqlite → DatabasePool → bootstrap 这条最小垂直链路验证 ABI，最后横向铺开其余插件与 Lua。`shield_plugin` 库重定义为插件系统核心，依赖方向反转为 `shield_data → shield_plugin`。

**Tech Stack:** C++17、nlohmann-json（manifest/schema）、yaml-cpp（主配置，仅读取 plugins 子树）、Boost.Test（单测）、CTest、CMake、跨平台 dlopen/LoadLibrary。

**关联文档:**
- 权威设计：`docs/plugin-system.md`
- 设计 spec：`docs/superpowers/specs/2026-06-22-plugin-system-v1-refactor-design.md`

---

## 重要约束（执行前必读）

1. **不执行 git 提交/分支操作**（用户指令）。每个任务以「验证检查点」收尾，不写 commit 步骤。
2. **测试框架是 Boost.Test**。测试注册模式见 `tests/CMakeLists.txt`：`add_executable` + `target_link_libraries(... PRIVATE <lib> Boost::unit_test_framework Boost::headers)` + `add_test` + `shield_copy_runtime_dlls`。
3. **CMake 依赖反转**（M0 关键）：现状 `shield_plugin → shield_data` 形成隐患；新设计 DatabasePool（shield_data）要调 PluginHost（shield_plugin）。M0 把 `shield_plugin` 重定义为只依赖 `shield_base/log/config`，`shield_data` 改为依赖 `shield_plugin`。
4. **配置格式**：外部配置统一用 YAML；插件 manifest 固定为 `manifest.yaml`；`plugins.instances/bindings/config` 作为 YAML 子树嵌在 `config/app.yaml`，进入插件系统后按 JSON-compatible 值模型处理。不动 `Config` 子系统、不迁 8 个 profile。
5. **redis**：废除 `plugins/redis/shield_redis.cpp` 公共插件；cache/queue/leaderboard 三件套各自内嵌 redis 连接（hiredis，shield_data 已有该依赖可参考）。

---

## 文件结构

### 新建

| 文件 | 职责 |
|---|---|
| `include/shield/plugin/abi.h` | `shield_plugin_abi_v1` / `instance_v1` / `create_args` / entry 声明 |
| `include/shield/plugin/host_api.h` | `shield_host_api_v1` / `context` / `shield_error_v1` / `shield_log_level` |
| `include/shield/plugin/database.h` | `shield_database_v1` + `shield_db_conn/result/connect_args`（从旧 db_plugin.h 搬） |
| `include/shield/plugin/cache.h` | `shield_cache_v1`（从旧 cache_plugin.h 演化） |
| `include/shield/plugin/queue.h` | `shield_queue_v1` |
| `include/shield/plugin/leaderboard.h` | `shield_leaderboard_v1` |
| `include/shield/plugin/auth.h` | `shield_auth_v1` |
| `include/shield/plugin/metrics.h` | `shield_metrics_v1` |
| `include/shield/plugin/health.h` | `shield_health_v1` |
| `include/shield/plugin/plugin_host.hpp` | C++ 数据模型 + `PluginHost` 类 + `global_host()` |
| `src/plugin/plugin_library.hpp/.cpp` | 跨平台 dlopen 封装（从旧 plugin_manager.cpp 搬） |
| `src/plugin/manifest.cpp` | manifest.yaml → `Manifest` 反序列化 + 校验 |
| `src/plugin/schema_validator.hpp/.cpp` | JSON Schema 最小子集校验 |
| `src/plugin/plugin_config.cpp` | app.yaml `plugins` 子树 → `PluginConfig` |
| `src/plugin/plugin_host.cpp` | scan/catalog/plan/resolve/load/create/start pipeline |
| `tests/plugin/test_manifest.cpp` | manifest 解析单测 |
| `tests/plugin/test_schema_validator.cpp` | schema 校验单测 |
| `tests/plugin/test_plugin_host.cpp` | pipeline 各阶段单测 |
| `tests/plugin/test_lua_plugin_api.cpp` | Lua introspection 单测 |
| `plugins/sqlite/manifest.yaml` | sqlite manifest |
| 其余插件各加 `manifest.yaml` | M5 |

### 修改

| 文件 | 改动 |
|---|---|
| `plugins/sqlite/shield_db_sqlite.cpp` | 迁移到 `shield_plugin_get_v1` + `shield_database_v1` |
| `plugins/sqlite/CMakeLists.txt` | 产物布局 + 部署 manifest.yaml |
| `src/data/data.cpp` | DatabasePool 改走 `get_by_binding<shield_database_v1>` |
| `src/bootstrap/bootstrap.cpp` | 集成 PluginHost startup/shutdown |
| `src/lua/lua_api.cpp` | 重写 `register_plugin_api`（:2185） |
| `CMakeLists.txt` | shield_plugin 重定义源 + 反转依赖；tests/plugin 注册 |
| `plugins/CMakeLists.txt` | target 名/布局 |
| `config/app.yaml` | 新增 `plugins:` 段（sqlite 内存库，开发/测试用） |

### 删除

| 文件 | 时机 |
|---|---|
| `include/shield/plugin/plugin.h` | M0（新头就位后） |
| `include/shield/plugin/db_plugin.h` | M0（内容并入 database.h） |
| `include/shield/core/plugin_manager.hpp` | M0 |
| `src/core/plugin_manager.cpp` | M0 |
| `include/shield/plugin/cache_plugin.h` / `redis_plugin.h` / `leaderboard_plugin.h` / `matchmaking_plugin.h` / `metric_plugin.h` / `health_plugin.h` / `auth_plugin.h` / `queue_plugin.h` | M5/M6（被新 interface 头取代，迁移对应插件后删） |
| `plugins/redis/shield_redis.cpp` | M6 |

### 任务依赖图

```
M0 (ABI 头 + CMake 反转) ── M1 (核心: manifest/schema/config/library)
                                                        │
                                                        ▼
                              M2 (PluginHost pipeline + host_api)
                                                        │
                                                        ▼
                              M3 (sqlite + DatabasePool + bootstrap) ← 纵切验收
                                                        │
                              ┌─────────────┬──────────┴──────────┐
                              ▼             ▼                     ▼
                        M4 (Lua)     M5 (横向插件)          M6 (redis 重构)
```

---

# M0 — ABI 头文件族 + CMake 依赖反转

**目标**：建立新 C ABI 头文件族，反转库依赖方向，删除旧 plugin 头/PluginManager。M0 完成后项目可编译（旧插件暂时编译失败没关系，M3+ 逐个迁移；但核心库必须编译）。

> ⚠️ 删除旧 `plugin.h`/`db_plugin.h` 会导致现有 8 个插件 .cpp 编译失败（它们 `#include` 这些头）。策略：**先建新头，M0 末尾删除旧头时，同时把 8 个插件的 `#include` 暂时改为引用新头但保留旧入口**——不，更干净的做法是 M0 只建新头 + 反转 CMake + 删 PluginManager，**旧头（plugin.h/db_plugin.h）保留到 M5/M6 迁移完所有插件后再删**。这样 M0-M3 期间插件仍可编译。修正如下。

## Task 0.1：创建 `abi.h`

**Files:**
- Create: `include/shield/plugin/abi.h`

- [ ] **Step 1：写头文件**

```c
// Shield Plugin System v1 — core ABI.
// Stable C entry: shield_plugin_get_v1() → shield_plugin_abi_v1.
// Interface-name based; no global type enum.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
  #define SHIELD_PLUGIN_EXPORT __declspec(dllexport)
#else
  #define SHIELD_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define SHIELD_PLUGIN_ABI_VERSION 1

// Forward declarations (defined in host_api.h)
struct shield_host_api_v1;
struct shield_error_v1;
struct shield_plugin_context_v1;

// Unified instance shell. Host only knows these three methods.
struct shield_plugin_instance_v1 {
    uint32_t struct_size;
    const char* instance_id;
    const void* (*get_interface)(struct shield_plugin_instance_v1* self,
                                 const char* interface_name,
                                 struct shield_error_v1* err);
    int  (*start)(struct shield_plugin_instance_v1* self,
                  struct shield_error_v1* err);
    void (*shutdown)(struct shield_plugin_instance_v1* self);
};

struct shield_plugin_create_args_v1 {
    const struct shield_host_api_v1* host_api;
    struct shield_plugin_context_v1* ctx;     // opaque, host-allocated
    const char* instance_id;
    const char* config_json;                  // validated instance config
};

struct shield_plugin_abi_v1 {
    uint32_t abi_version;     // == SHIELD_PLUGIN_ABI_VERSION
    uint32_t struct_size;     // host checks >= minimum
    const char* package_id;   // must match manifest.yaml id
    const char* package_version;
    int (*create)(const struct shield_plugin_create_args_v1* args,
                  struct shield_plugin_instance_v1** out,
                  struct shield_error_v1* err);
};

SHIELD_PLUGIN_EXPORT
const struct shield_plugin_abi_v1* shield_plugin_get_v1(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2：验证头可独立编译**

Run: `echo '#include "shield/plugin/abi.h"' | clang++ -std=c++17 -fsyntax-only -x c++ -Iinclude -`
Expected: 无错误。

## Task 0.2：创建 `host_api.h`

**Files:**
- Create: `include/shield/plugin/host_api.h`

- [ ] **Step 1：写头文件**

```c
// Shield Plugin System v1 — host API + error + context.
#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum shield_log_level {
    SHIELD_LOG_DEBUG = 0,
    SHIELD_LOG_INFO  = 1,
    SHIELD_LOG_WARN  = 2,
    SHIELD_LOG_ERROR = 3,
};

struct shield_error_v1 {
    const char* code;         // "plugin.abi.mismatch"
    const char* message;
    const char* hint;
    const char* package_id;
    const char* instance_id;
    const char* phase;        // scan|catalog|plan|resolve|load|create|start
};

// Opaque context. Host allocates; plugin uses it to call back into host
// (config_get / dependency). Layout is host-private.
struct shield_plugin_context_v1;

struct shield_host_api_v1 {
    void (*log)(enum shield_log_level level,
                const char* package_id,
                const char* instance_id,
                const char* message);
    void (*report_error)(const struct shield_error_v1* err);
    // Dot-path lookup into the instance's validated config. Returns JSON
    // fragment string (host-owned, valid until next call) or NULL.
    const char* (*config_get)(struct shield_plugin_context_v1* ctx,
                              const char* path);
    // Resolved dependency by require-name + interface. Host verifies the
    // name is in this instance's manifest.requires and the target provides
    // the interface. Returns vtable pointer or NULL.
    const void* (*dependency)(struct shield_plugin_context_v1* ctx,
                              const char* name,
                              const char* interface_name);
};

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2：验证编译**

Run: `echo '#include "shield/plugin/host_api.h"' | clang++ -std=c++17 -fsyntax-only -x c++ -Iinclude -`
Expected: 无错误。

## Task 0.3：创建 `database.h`（搬运 + 演化）

**Files:**
- Create: `include/shield/plugin/database.h`

将旧 `db_plugin.h` 的 `shield_db_conn/result/connect_args` 原样保留，`shield_db_plugin` 演化为 `shield_database_v1`（`abi_version` → `struct_size`）。

- [ ] **Step 1：写头文件**

```c
// Shield Plugin System v1 — shield.database.v1 interface.
// Function signatures inherited verbatim from the legacy shield_db_plugin
// (db_plugin.h) so DatabasePool migration is a type-rename.
#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_DATABASE_INTERFACE "shield.database.v1"

struct shield_db_conn;  // opaque, plugin-defined

struct shield_db_connect_args {
    const char* host;
    int         port;
    const char* user;
    const char* password;
    const char* database;     // db name OR file path (sqlite)
    const char* extra_json;   // driver-specific JSON, may be NULL
    int connect_timeout_ms;
    int query_timeout_ms;
};

struct shield_db_result {
    int      success;
    const char* error_msg;
    const char* error_code;
    int64_t  affected_rows;
    int64_t  last_insert_id;
    int      row_count;
    int      col_count;
    const char** cells;
};

struct shield_database_v1 {
    uint32_t struct_size;                       // == sizeof(shield_database_v1)
    const char* name;                           // "sqlite"|"mysql"|...
    const char* version;

    struct shield_db_conn* (*connect)(const struct shield_db_connect_args* args,
                                      char* err_buf, int err_buf_size);
    void (*disconnect)(struct shield_db_conn* conn);
    int  (*ping)(struct shield_db_conn* conn);

    int (*query)(struct shield_db_conn* conn, const char* sql,
                 const char* const* params, int n_params,
                 struct shield_db_result* out_result);
    int (*execute)(struct shield_db_conn* conn, const char* sql,
                   const char* const* params, int n_params,
                   struct shield_db_result* out_result);

    int (*begin)(struct shield_db_conn* conn, struct shield_db_result* out_result);
    int (*commit)(struct shield_db_conn* conn, struct shield_db_result* out_result);
    int (*rollback)(struct shield_db_conn* conn, struct shield_db_result* out_result);

    void (*free_result)(struct shield_db_result* result);
};

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2：验证编译**

Run: `echo '#include "shield/plugin/database.h"' | clang++ -std=c++17 -fsyntax-only -x c++ -Iinclude -`
Expected: 无错误。

## Task 0.4：CMake 依赖反转 + shield_plugin 重定义

**Files:**
- Modify: `CMakeLists.txt:356-368`（shield_plugin）、`CMakeLists.txt:398-401`（shield_data）

- [ ] **Step 1：重定义 shield_plugin 源与依赖**（去掉对 shield_data 的依赖，源改为空占位，M1 起填充）

把 `CMakeLists.txt:356-368` 替换为：

```cmake
# -----------------------------------------------------------------------------
# shield_plugin - Plugin system v1 (manifest/catalog/pipeline/host)
# NOTE: depends only on base/log/config. shield_data depends ON this (reverse
# of the old direction) so DatabasePool can call PluginHost::get_by_binding.
# -----------------------------------------------------------------------------
add_library(shield_plugin STATIC
    src/plugin/plugin_library.cpp
)

target_include_directories(shield_plugin
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_link_libraries(shield_plugin
    PUBLIC shield_base shield_log shield_config
    PRIVATE nlohmann_json::nlohmann_json
)
```

> 注：`src/plugin/plugin_library.cpp` 在 Task 1.1 创建。M0 此步先建空文件占位（见 Step 2）以保证 CMake 配置通过。

- [ ] **Step 2：创建占位源文件**（Task 1.1 会填充）

Create `src/plugin/plugin_library.cpp`：
```cpp
// Placeholder — filled in Task 1.1.
```

- [ ] **Step 3：shield_data 增加 shield_plugin 依赖**

把 `CMakeLists.txt:398-401` 的 `target_link_libraries(shield_data ...)` 改为：

```cmake
target_link_libraries(shield_data
    PUBLIC shield_base shield_log shield_config shield_plugin
    PRIVATE redis++::redis++ hiredis::hiredis
)
```

- [ ] **Step 4：删除旧 PluginManager 源与头**

删除：
- `src/core/plugin_manager.cpp`
- `include/shield/core/plugin_manager.hpp`

- [ ] **Step 5：确认无残留引用**

Run: `rg -n "plugin_manager|PluginManager" src include --glob '!docs/**'`
Expected: 无输出（或仅注释，需清理）。

- [ ] **Step 6：验证 CMake 配置与核心库编译**

Run: `cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -20 && cmake --build build-debug --target shield_plugin shield_data shield_core 2>&1 | tail -30`
Expected: 三个目标编译通过（旧插件 .so 目标此时仍可能失败，不在本步范围；仅验证核心库）。

**验证检查点：** 新三个 ABI 头可独立编译；`shield_plugin`/`shield_data`/`shield_core` 编译通过；旧 PluginManager 已删且无残留引用。

---

# M1 — 插件系统核心：Library / Manifest / Schema / Config

## Task 1.1：PluginLibrary（搬运 + 测试）

**Files:**
- Create: `src/plugin/plugin_library.hpp`
- Modify: `src/plugin/plugin_library.cpp`（填充占位）
- Test: `tests/plugin/test_plugin_library.cpp`

- [ ] **Step 1：写头文件** `src/plugin/plugin_library.hpp`

```cpp
#pragma once
#include <string>

namespace shield::plugin {

// Cross-platform dlopen/LoadLibrary wrapper. Self-contained (no dependency
// on shield_data::detail::DynamicLibrary) to keep shield_plugin leaf-level.
class PluginLibrary {
public:
    PluginLibrary() = default;
    ~PluginLibrary();
    PluginLibrary(const PluginLibrary&) = delete;
    PluginLibrary& operator=(const PluginLibrary&) = delete;
    PluginLibrary(PluginLibrary&&) noexcept;
    PluginLibrary& operator=(PluginLibrary&&) noexcept;

    static PluginLibrary load(const std::string& path, std::string& error);
    bool is_loaded() const;
    void* resolve(const char* symbol) const;

private:
    void close();
    void* handle_ = nullptr;
};

}  // namespace shield::plugin
```

- [ ] **Step 2：写实现**（从旧 `plugin_manager.cpp:23-85` 搬运，逻辑不变）

```cpp
#include "plugin_library.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace shield::plugin {

PluginLibrary::~PluginLibrary() { close(); }

PluginLibrary::PluginLibrary(PluginLibrary&& o) noexcept : handle_(o.handle_) {
    o.handle_ = nullptr;
}
PluginLibrary& PluginLibrary::operator=(PluginLibrary&& o) noexcept {
    if (this != &o) { close(); handle_ = o.handle_; o.handle_ = nullptr; }
    return *this;
}

PluginLibrary PluginLibrary::load(const std::string& path, std::string& error) {
    PluginLibrary lib;
#ifdef _WIN32
    lib.handle_ = LoadLibraryA(path.c_str());
    if (!lib.handle_) error = "LoadLibrary failed: " + std::to_string(GetLastError());
#else
    lib.handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib.handle_) error = dlerror() ? dlerror() : "dlopen failed";
#endif
    return lib;
}

bool PluginLibrary::is_loaded() const { return handle_ != nullptr; }

void* PluginLibrary::resolve(const char* symbol) const {
    if (!handle_) return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), symbol));
#else
    return dlsym(handle_, symbol);
#endif
}

void PluginLibrary::close() {
    if (handle_) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(handle_));
#else
        dlclose(handle_);
#endif
        handle_ = nullptr;
    }
}

}  // namespace shield::plugin
```

- [ ] **Step 3：写测试** `tests/plugin/test_plugin_library.cpp`

```cpp
// NOTE: this test loads the host executable itself (which exports symbols
// on most platforms) as a smoke test of resolve(). Skip if not feasible.
#define BOOST_TEST_MODULE shield_plugin_library
#include <boost/test/included/unit_test.hpp>
#include "plugin_library.hpp"

BOOST_AUTO_TEST_CASE(load_missing_file_fails) {
    std::string err;
    auto lib = shield::plugin::PluginLibrary::load(
        "/nonexistent/path/libnope.so", err);
    BOOST_CHECK(!lib.is_loaded());
    BOOST_CHECK(!err.empty());
}
```

- [ ] **Step 4：注册测试到 tests/CMakeLists.txt**

在 `tests/CMakeLists.txt` 末尾追加：

```cmake
# Plugin library loading tests
add_executable(test_plugin_library plugin/test_plugin_library.cpp)
target_link_libraries(test_plugin_library
    PRIVATE
        shield_plugin
        Boost::unit_test_framework
        Boost::headers
)
target_include_directories(test_plugin_library
    PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/plugin
)
add_test(NAME test_plugin_library COMMAND test_plugin_library)
set_tests_properties(test_plugin_library PROPERTIES LABELS "plugin" TIMEOUT 10)
shield_copy_runtime_dlls(test_plugin_library)
```

- [ ] **Step 5：运行测试**

Run: `cmake --build build-debug --target test_plugin_library 2>&1 | tail && ctest --test-dir build-debug -R test_plugin_library --output-on-failure`
Expected: PASS。

**验证检查点：** PluginLibrary 可用；测试注册并通过。

## Task 1.2：Manifest 解析（manifest.yaml → Manifest）

**Files:**
- Create: `include/shield/plugin/plugin_host.hpp`（数据模型部分，先放 Manifest/InstanceDecl/BindingDecl/State）
- Create: `src/plugin/manifest.cpp`
- Test: `tests/plugin/test_manifest.cpp`

- [ ] **Step 1：在 plugin_host.hpp 定义数据模型**

```cpp
#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace shield::plugin {

struct Manifest {
    int schema_version = 1;
    std::string id, name, version, kind, description, entry;
    struct Lib { std::string windows, linux, macos; } library;
    struct Provide { std::string interface; std::vector<std::string> capabilities; };
    std::vector<Provide> provides;
    struct Require { std::string name, interface; bool optional = false; };
    std::vector<Require> requires_;
    nlohmann::json config_schema;
};

struct InstanceDecl {
    std::string id, package;
    bool required = true;
    std::map<std::string, std::string> dependencies;  // name -> instance id
    nlohmann::json config;
};

struct BindingDecl { std::string logical, instance_id; };

struct PluginConfig {
    std::string directory = "./plugins";
    std::vector<InstanceDecl> instances;
    std::vector<BindingDecl> bindings;
};

// Parse + validate a manifest JSON. Throws std::runtime_error on invalid.
Manifest parse_manifest(const nlohmann::json& j);
Manifest load_manifest_file(const std::filesystem::path& plugin_json);

// Resolve platform library path from manifest.
std::string platform_library_path(const Manifest& m);

}  // namespace shield::plugin
```

- [ ] **Step 2：写测试** `tests/plugin/test_manifest.cpp`（TDD：先写失败测试）

```cpp
#define BOOST_TEST_MODULE shield_plugin_manifest
#include <boost/test/included/unit_test.hpp>
#include "shield/plugin/plugin_host.hpp"
#include <nlohmann/json.hpp>

using namespace shield::plugin;
using json = nlohmann::json;

BOOST_AUTO_TEST_CASE(parse_minimal_manifest) {
    json j = {
        {"schema_version", 1}, {"id", "database.sqlite"},
        {"name", "SQLite"}, {"version", "1.0.0"}, {"kind", "database"},
        {"entry", "shield_plugin_get_v1"},
        {"library", {{"linux", "bin/libshield_database_sqlite.so"}}},
        {"provides", json::array({{{"interface", "shield.database.v1"},
                                    {"capabilities", json::array({"sql"})}}})},
        {"requires", json::array()},
        {"config_schema", {{"type", "object"}}}
    };
    auto m = parse_manifest(j);
    BOOST_CHECK_EQUAL(m.id, "database.sqlite");
    BOOST_CHECK_EQUAL(m.entry, "shield_plugin_get_v1");
    BOOST_REQUIRE_EQUAL(m.provides.size(), 1u);
    BOOST_CHECK_EQUAL(m.provides[0].interface, "shield.database.v1");
}

BOOST_AUTO_TEST_CASE(rejects_missing_id) {
    json j = {{"schema_version", 1}};
    BOOST_CHECK_THROW(parse_manifest(j), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(rejects_wrong_schema_version) {
    json j = {{"schema_version", 2}, {"id", "x"}, {"entry", "shield_plugin_get_v1"},
              {"library", {{"linux", "x.so"}}}, {"provides", json::array()},
              {"requires", json::array()}, {"config_schema", json::object()}};
    BOOST_CHECK_THROW(parse_manifest(j), std::runtime_error);
}
```

- [ ] **Step 3：运行测试，确认失败**

Run: `cmake --build build-debug --target test_manifest 2>&1 | tail`（需先在 CMake 注册，见 Step 5）
Expected: 链接失败（`parse_manifest` 未定义）。

- [ ] **Step 4：写实现** `src/plugin/manifest.cpp`

```cpp
#include "shield/plugin/plugin_host.hpp"
#include <fstream>
#include <sstream>

namespace shield::plugin {

namespace {
void require(const nlohmann::json& j, const char* key, const std::string& ctx) {
    if (!j.contains(key) || j[key].is_null())
        throw std::runtime_error("manifest " + ctx + ": missing '" + std::string(key) + "'");
}
}

Manifest parse_manifest(const nlohmann::json& j) {
    Manifest m;
    require(j, "schema_version", "");
    m.schema_version = j.at("schema_version").get<int>();
    if (m.schema_version != 1)
        throw std::runtime_error("manifest: schema_version must be 1");
    require(j, "id", "");          m.id = j.at("id").get<std::string>();
    require(j, "entry", "");       m.entry = j.at("entry").get<std::string>();
    m.name = j.value("name", m.id);
    m.version = j.value("version", std::string("0.0.0"));
    m.kind = j.value("kind", std::string());
    m.description = j.value("description", std::string());

    require(j, "library", "");
    const auto& lib = j.at("library");
    m.library.linux   = lib.value("linux",   std::string());
    m.library.macos   = lib.value("macos",   std::string());
    m.library.windows = lib.value("windows", std::string());

    require(j, "provides", "");
    for (const auto& p : j.at("provides")) {
        Manifest::Provide pr;
        require(p, "interface", "provides");
        pr.interface = p.at("interface").get<std::string>();
        if (p.contains("capabilities"))
            pr.capabilities = p.at("capabilities").get<std::vector<std::string>>();
        m.provides.push_back(std::move(pr));
    }
    if (j.contains("requires")) {
        for (const auto& r : j.at("requires")) {
            Manifest::Require rq;
            require(r, "name", "requires");       rq.name = r.at("name").get<std::string>();
            require(r, "interface", "requires");  rq.interface = r.at("interface").get<std::string>();
            rq.optional = r.value("optional", false);
            m.requires_.push_back(std::move(rq));
        }
    }
    m.config_schema = j.value("config_schema", nlohmann::json::object());
    return m;
}

Manifest load_manifest_file(const std::filesystem::path& plugin_json) {
    std::ifstream f(plugin_json);
    if (!f) throw std::runtime_error("cannot open " + plugin_json.string());
    std::stringstream ss; ss << f.rdbuf();
    return parse_manifest(nlohmann::json::parse(ss.str()));
}

std::string platform_library_path(const Manifest& m) {
#if defined(_WIN32)
    return m.library.windows;
#elif defined(__APPLE__)
    return m.library.macos;
#else
    return m.library.linux;
#endif
}

}  // namespace shield::plugin
```

- [ ] **Step 5：把 manifest.cpp 加入 shield_plugin 源，注册测试**

修改 `CMakeLists.txt` 的 `add_library(shield_plugin STATIC ...)`，在 `src/plugin/plugin_library.cpp` 下加 `src/plugin/manifest.cpp`。

在 `tests/CMakeLists.txt` 追加（同 Task 1.1 模式，链接 `shield_plugin`，include 加 `${CMAKE_SOURCE_DIR}/src/plugin`）：

```cmake
add_executable(test_manifest plugin/test_manifest.cpp)
target_link_libraries(test_manifest PRIVATE shield_plugin nlohmann_json::nlohmann_json
    Boost::unit_test_framework Boost::headers)
target_include_directories(test_manifest PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/plugin)
add_test(NAME test_manifest COMMAND test_manifest)
set_tests_properties(test_manifest PROPERTIES LABELS "plugin" TIMEOUT 10)
shield_copy_runtime_dlls(test_manifest)
```

- [ ] **Step 6：运行测试**

Run: `cmake --build build-debug --target test_manifest 2>&1 | tail && ctest --test-dir build-debug -R test_manifest --output-on-failure`
Expected: 3 个 case 全 PASS。

**验证检查点：** manifest 解析正确接受合法 JSON、拒绝缺字段/错误 schema_version。

## Task 1.3：schema_validator（config_schema 校验）

**Files:**
- Create: `src/plugin/schema_validator.hpp`
- Create: `src/plugin/schema_validator.cpp`
- Test: `tests/plugin/test_schema_validator.cpp`

- [ ] **Step 1：头文件** `src/plugin/schema_validator.hpp`

```cpp
#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace shield::plugin {

// Validate `value` against a JSON-Schema subset. Returns empty string on
// success, or a human-readable error path on failure.
// Supported keywords: type, required, properties, default, minimum, maximum,
// enum, items, secret (recognized but not enforced here — used for masking).
std::string validate_config(const nlohmann::json& schema,
                            const nlohmann::json& value,
                            const std::string& path = "");

// Apply schema defaults into value (mutates in place).
void apply_defaults(const nlohmann::json& schema, nlohmann::json& value);

}  // namespace shield::plugin
```

- [ ] **Step 2：测试** `tests/plugin/test_schema_validator.cpp`

```cpp
#define BOOST_TEST_MODULE shield_plugin_schema
#include <boost/test/included/unit_test.hpp>
#include "schema_validator.hpp"
using namespace shield::plugin;
using json = nlohmann::json;

BOOST_AUTO_TEST_CASE(type_mismatch) {
    json schema = {{"type","object"},{"properties",{{"port",{{"type","integer"},{"minimum",1},{"maximum",65535}}}}}},
                   {"required", json::array({"port"})}};
    json val = {{"port", "notanumber"}};
    BOOST_CHECK(!validate_config(schema, val, "").empty());
}

BOOST_AUTO_TEST_CASE(required_missing) {
    json schema = {{"type","object"},{"required", json::array({"host"})}};
    json val = json::object();
    BOOST_CHECK(!validate_config(schema, val, "").empty());
}

BOOST_AUTO_TEST_CASE(range_violation) {
    json schema = {{"type","object"},{"properties",{{"port",{{"type","integer"},{"minimum",1},{"maximum",10}}}}}}; 
    json val = {{"port", 99}};
    BOOST_CHECK(!validate_config(schema, val, "").empty());
}

BOOST_AUTO_TEST_CASE(valid_passes) {
    json schema = {{"type","object"},{"properties",{{"port",{{"type","integer"},{"default",3306}}}}}}; 
    json val = {{"port", 3306}};
    BOOST_CHECK(validate_config(schema, val, "").empty());
}

BOOST_AUTO_TEST_CASE(apply_defaults_fills_missing) {
    json schema = {{"type","object"},{"properties",{{"port",{{"default",3306}}}}}}; 
    json val = json::object();
    apply_defaults(schema, val);
    BOOST_CHECK_EQUAL(val["port"].get<int>(), 3306);
}
```

- [ ] **Step 3：运行确认失败**（实现未写）

- [ ] **Step 4：实现** `src/plugin/schema_validator.cpp`

```cpp
#include "schema_validator.hpp"

namespace shield::plugin {

static bool check_type(const std::string& want, const nlohmann::json& v) {
    if (want == "object")    return v.is_object();
    if (want == "array")     return v.is_array();
    if (want == "string")    return v.is_string();
    if (want == "integer")   return v.is_number_integer();
    if (want == "number")    return v.is_number();
    if (want == "boolean")   return v.is_boolean();
    return true;  // unknown types: lenient
}

std::string validate_config(const nlohmann::json& schema,
                            const nlohmann::json& value,
                            const std::string& path) {
    if (schema.contains("type")) {
        const auto t = schema.at("type").get<std::string>();
        if (!check_type(t, value))
            return path.empty() ? ("type mismatch: expected " + t) : (path + ": type mismatch");
    }
    if (value.is_object()) {
        if (schema.contains("required")) {
            for (const auto& k : schema.at("required"))
                if (!value.contains(k.get<std::string>()))
                    return (path.empty() ? std::string() : path + ".") + k.get<std::string>() + ": required";
        }
        if (schema.contains("properties")) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (schema.at("properties").contains(it.key())) {
                    auto err = validate_config(schema.at("properties").at(it.key()), it.value(),
                                               (path.empty() ? std::string() : path + ".") + it.key());
                    if (!err.empty()) return err;
                }
            }
        }
    }
    if (value.is_number() && schema.contains("minimum") && value.get<double>() < schema.at("minimum").get<double>())
        return path + ": below minimum";
    if (value.is_number() && schema.contains("maximum") && value.get<double>() > schema.at("maximum").get<double>())
        return path + ": above maximum";
    if (schema.contains("enum")) {
        bool ok = false;
        for (const auto& e : schema.at("enum")) if (e == value) { ok = true; break; }
        if (!ok) return path + ": not in enum";
    }
    if (value.is_array() && schema.contains("items")) {
        size_t i = 0;
        for (const auto& el : value) {
            auto err = validate_config(schema.at("items"), el, path + "[" + std::to_string(i++) + "]");
            if (!err.empty()) return err;
        }
    }
    return {};
}

void apply_defaults(const nlohmann::json& schema, nlohmann::json& value) {
    if (!value.is_object() || !schema.contains("properties")) return;
    for (auto it = schema.at("properties").begin(); it != schema.at("properties").end(); ++it) {
        const auto& key = it.key();
        const auto& sub = it.value();
        if (!value.contains(key) && sub.contains("default"))
            value[key] = sub.at("default");
        else if (value.contains(key) && value[key].is_object())
            apply_defaults(sub, value[key]);
    }
}

}  // namespace shield::plugin
```

- [ ] **Step 5：加入 shield_plugin 源 + 注册测试**

`add_library(shield_plugin STATIC ...)` 追加 `src/plugin/schema_validator.cpp`。`tests/CMakeLists.txt` 追加 test_schema_validator（模式同前）。

- [ ] **Step 6：运行测试**

Run: `cmake --build build-debug --target test_schema_validator 2>&1 | tail && ctest --test-dir build-debug -R test_schema_validator --output-on-failure`
Expected: 5 case 全 PASS。

**验证检查点：** schema 校验覆盖 type/required/range/enum/default。

## Task 1.4：PluginConfig 解析（app.yaml plugins 子树）

**Files:**
- Modify: `include/shield/plugin/plugin_host.hpp`（加 `load_plugin_config` 声明）
- Create: `src/plugin/plugin_config.cpp`
- Test: `tests/plugin/test_plugin_config.cpp`

- [ ] **Step 1：声明 loader**

在 `plugin_host.hpp` 追加：

```cpp
// Load PluginConfig from the global shield::config (app.yaml `plugins:` subtree).
PluginConfig load_plugin_config();
```

- [ ] **Step 2：测试** `tests/plugin/test_plugin_config.cpp`

```cpp
#define BOOST_TEST_MODULE shield_plugin_config
#include <boost/test/included/unit_test.hpp>
#include "shield/plugin/plugin_host.hpp"
#include "shield/config/config.hpp"

BOOST_AUTO_TEST_CASE(loads_instances_and_bindings) {
    using namespace shield::config;
    Config cfg;
    cfg.load_yaml_string(R"(
plugins:
  directory: ./plugins
  instances:
    - id: db.main
      package: database.sqlite
      required: true
      config: { database: ":memory:" }
  bindings:
    database.default: db.main
)");
    shield::config::set_global_config(cfg);   // helper if exists; else inject via test fixture
    auto pc = shield::plugin::load_plugin_config();
    BOOST_CHECK_EQUAL(pc.directory, "./plugins");
    BOOST_REQUIRE_EQUAL(pc.instances.size(), 1u);
    BOOST_CHECK_EQUAL(pc.instances[0].id, "db.main");
    BOOST_REQUIRE_EQUAL(pc.bindings.size(), 1u);
    BOOST_CHECK_EQUAL(pc.bindings[0].logical, "database.default");
}
```

> 注：若 `Config` 无 `set_global_config`，改为构造 `PluginConfig` 的纯函数 `parse_plugin_config(const shield::config::Config&)` 并在测试里直接传入，避免依赖全局态。**采用纯函数版本**（见实现）。

- [ ] **Step 3：调整声明为纯函数**

`plugin_host.hpp` 改为：
```cpp
PluginConfig parse_plugin_config(const class shield::config::Config& cfg);
PluginConfig load_plugin_config();  // wraps global_config()
```

- [ ] **Step 4：实现** `src/plugin/plugin_config.cpp`

```cpp
#include "shield/plugin/plugin_host.hpp"
#include "shield/config/config.hpp"

namespace shield::plugin {

PluginConfig parse_plugin_config(const shield::config::Config& cfg) {
    PluginConfig pc;
    pc.directory = cfg.get_string("plugins.directory", "./plugins");

    // instances: Config 不直接给数组遍历，借助 to_json() 拿 plugins 子树再解析
    nlohmann::json root = nlohmann::json::parse(cfg.to_json());
    if (!root.contains("plugins")) return pc;
    const auto& plugins = root["plugins"];

    if (plugins.contains("instances")) {
        for (const auto& in : plugins.at("instances")) {
            InstanceDecl d;
            d.id = in.value("id", std::string());
            d.package = in.value("package", std::string());
            d.required = in.value("required", true);
            if (in.contains("dependencies"))
                for (auto it = in.at("dependencies").begin(); it != in.at("dependencies").end(); ++it)
                    d.dependencies[it.key()] = it.value().get<std::string>();
            d.config = in.value("config", nlohmann::json::object());
            pc.instances.push_back(std::move(d));
        }
    }
    if (plugins.contains("bindings")) {
        for (auto it = plugins.at("bindings").begin(); it != plugins.at("bindings").end(); ++it) {
            BindingDecl b;
            b.logical = it.key();
            b.instance_id = it.value().get<std::string>();
            pc.bindings.push_back(std::move(b));
        }
    }
    return pc;
}

PluginConfig load_plugin_config() {
    return parse_plugin_config(shield::config::global_config());
}

}  // namespace shield::plugin
```

> 依赖 `shield::config::Config::to_json()`（已存在，见 `config.hpp:89`）。

- [ ] **Step 5：加入源 + 注册测试 + 运行**

`add_library(shield_plugin STATIC ...)` 加 `src/plugin/plugin_config.cpp`，其 `target_link_libraries` 已含 `shield_config`（PUBLIC）。测试链接追加 `yaml-cpp::yaml-cpp`。

Run: `cmake --build build-debug --target test_plugin_config 2>&1 | tail && ctest --test-dir build-debug -R test_plugin_config --output-on-failure`
Expected: PASS。

**验证检查点：** 能从 app.yaml `plugins:` 子树解析出 instances/bindings/config。

---

# M2 — PluginHost Pipeline + Host API

> 本阶段先在 `plugin_host.hpp` 补全 `PluginHost` 类与运行期结构（Package/Instance/State），再分阶段实现 pipeline。每阶段配 Boost.Test。

## Task 2.1：运行期数据结构 + scan/catalog

**Files:**
- Modify: `include/shield/plugin/plugin_host.hpp`（加 Package/Instance/State/PluginHost 声明）
- Create: `src/plugin/plugin_host.cpp`（scan/catalog 部分）
- Test: `tests/plugin/test_plugin_host.cpp`（scan/catalog 用例）

- [ ] **Step 1：补全 plugin_host.hpp**

```cpp
// 追加到 plugin_host.hpp
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "plugin_library.hpp"

namespace shield::plugin {

struct Package { Manifest manifest; std::filesystem::path root; };

enum class State { planned, loaded, started, unavailable, failed, stopped };

struct Instance {
    std::string id;
    const Package* package = nullptr;
    InstanceDecl decl;
    State state = State::planned;
    const shield_plugin_abi_v1* abi = nullptr;
    shield_plugin_instance_v1* handle = nullptr;
    PluginLibrary lib;
    std::vector<std::string> dep_ids;
    std::string last_error;  // 结构化错误 message（简化：code 拼接）
};

class PluginHost {
public:
    PluginHost();
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    bool startup(const PluginConfig& cfg, std::string& error);
    void shutdown();

    // —— 阶段（测试可单独调用）——
    void scan(const std::string& directory);          // 填充 packages_
    bool catalog(std::string& error);                 // 校验 packages_
    bool plan_and_resolve(const PluginConfig& cfg, std::string& error);

    // —— 查询 ——
    template<typename Interface>
    const Interface* get_by_binding(std::string_view binding_name) const;
    std::vector<std::string> package_ids() const;
    const Package* find_package(const std::string& id) const;
    const Instance* find_instance(const std::string& id) const;
    const std::vector<Instance>& instances() const { return instances_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<Package> packages_;
    std::vector<Instance> instances_;
};

PluginHost& global_host();
}  // namespace shield::plugin
```

> `get_by_binding` 模板实现放头文件（依赖 `Interface` 的 `interface_name`，约定每个 vtable 头定义 `static constexpr const char* interface_name`）。

- [ ] **Step 2：get_by_binding 模板实现（头文件内联）**

```cpp
// plugin_host.hpp 内
namespace shield::plugin {
template<typename Interface>
const Interface* PluginHost::get_by_binding(std::string_view name) const {
    // 查 binding -> instance id（impl_ 持有 bindings 表）
    return static_cast<const Interface*>(get_binding_vtable(name, Interface::interface_name));
}
}
```

> 需补 `const void* get_binding_vtable(std::string_view binding, const char* iface) const;` 私有成员声明。

- [ ] **Step 3：测试 scan/catalog** `tests/plugin/test_plugin_host.cpp`

```cpp
#define BOOST_TEST_MODULE shield_plugin_host
#include <boost/test/included/unit_test.hpp>
#include "shield/plugin/plugin_host.hpp"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

struct CatalogFixture {
    fs::path root;
    CatalogFixture() {
        root = fs::temp_directory_path() / "shield_plugin_test";
        fs::remove_all(root); fs::create_directories(root / "database.sqlite" / "bin");
        std::ofstream(root / "database.sqlite" / "manifest.yaml") << R"({
          "schema_version":1,"id":"database.sqlite","name":"SQLite","version":"1.0.0",
          "kind":"database","entry":"shield_plugin_get_v1",
          "library":{"linux":"bin/libshield_database_sqlite.so","macos":"bin/libshield_database_sqlite.dylib","windows":"bin/shield_database_sqlite.dll"},
          "provides":[{"interface":"shield.database.v1","capabilities":["sql"]}],
          "requires":[],"config_schema":{"type":"object"}
        })";
    }
    ~CatalogFixture() { fs::remove_all(root); }
};

BOOST_FIXTURE_TEST_CASE(scan_finds_package, CatalogFixture) {
    shield::plugin::PluginHost host;
    host.scan(root.string());
    auto ids = host.package_ids();
    BOOST_REQUIRE_EQUAL(ids.size(), 1u);
    BOOST_CHECK_EQUAL(ids[0], "database.sqlite");
}

BOOST_FIXTURE_TEST_CASE(catalog_rejects_duplicate_id, CatalogFixture) {
    fs::create_directories(root / "database.sqlite.dup");
    std::ofstream(root / "database.sqlite.dup" / "manifest.yaml") << R"({"schema_version":1,"id":"database.sqlite","entry":"shield_plugin_get_v1","library":{"linux":"x.so"},"provides":[],"requires":[]})";
    shield::plugin::PluginHost host;
    host.scan(root.string());
    std::string err;
    BOOST_CHECK(!host.catalog(err));
    BOOST_TEST(err.find("duplicate") != std::string::npos);
}
```

- [ ] **Step 4：运行确认失败**

- [ ] **Step 5：实现 plugin_host.cpp 的 scan/catalog**

```cpp
#include "shield/plugin/plugin_host.hpp"

namespace shield::plugin {

namespace fs = std::filesystem;

struct PluginHost::Impl {};

PluginHost::PluginHost() : impl_(std::unique_ptr<Impl>(new Impl)) {}
PluginHost::~PluginHost() { shutdown(); }

void PluginHost::scan(const std::string& directory) {
    fs::path dir(directory);
    std::error_code ec;
    if (!fs::exists(dir) || !fs::is_directory(dir)) return;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_directory()) continue;
        fs::path json_path = entry.path() / "manifest.yaml";
        if (!fs::exists(json_path)) continue;
        try {
            Package pkg;
            pkg.manifest = load_manifest_file(json_path);
            pkg.root = entry.path();
            packages_.push_back(std::move(pkg));
        } catch (const std::exception&) {
            // catalog 阶段统一报错；scan 静默跳过坏 manifest
        }
    }
}

bool PluginHost::catalog(std::string& error) {
    std::set<std::string> seen;
    for (const auto& p : packages_) {
        if (!seen.insert(p.manifest.id).second) {
            error = "plugin.manifest.invalid: duplicate package id '" + p.manifest.id + "'";
            return false;
        }
        if (platform_library_path(p.manifest).empty()) {
            error = "plugin.manifest.invalid: package '" + p.manifest.id +
                    "' missing library path for current platform";
            return false;
        }
    }
    return true;
}

std::vector<std::string> PluginHost::package_ids() const {
    std::vector<std::string> v;
    for (const auto& p : packages_) v.push_back(p.manifest.id);
    return v;
}
const Package* PluginHost::find_package(const std::string& id) const {
    for (const auto& p : packages_) if (p.manifest.id == id) return &p;
    return nullptr;
}
const Instance* PluginHost::find_instance(const std::string& id) const {
    for (const auto& i : instances_) if (i.id == id) return &i;
    return nullptr;
}

}  // namespace shield::plugin
```

> 文件顶部补 `#include <set>`。

- [ ] **Step 6：加入源 + 注册 test_plugin_host + 运行**

`add_library(shield_plugin STATIC ...)` 加 `src/plugin/plugin_host.cpp`。注册 test_plugin_host（链接 shield_plugin）。

Run: `cmake --build build-debug --target test_plugin_host 2>&1 | tail && ctest --test-dir build-debug -R test_plugin_host -R scan -R catalog --output-on-failure`
Expected: scan/catalog 用例 PASS。

**验证检查点：** scan 发现包、catalog 校验 id 唯一与平台库路径。

## Task 2.2：plan + resolve（含拓扑排序 + 循环检测）

**Files:**
- Modify: `src/plugin/plugin_host.cpp`（实现 plan_and_resolve）
- Modify: `tests/plugin/test_plugin_host.cpp`

- [ ] **Step 1：测试 plan/resolve/拓扑/循环**

```cpp
BOOST_FIXTURE_TEST_CASE(resolve_missing_package_fails, CatalogFixture) {
    shield::plugin::PluginHost host; host.scan(root.string());
    std::string err;
    shield::plugin::PluginConfig cfg;
    cfg.directory = root.string();
    shield::plugin::InstanceDecl in; in.id = "db.main"; in.package = "database.nope"; in.required = true;
    cfg.instances.push_back(in);
    BOOST_CHECK(!host.plan_and_resolve(cfg, err));
    BOOST_TEST(err.find("plugin.package.not_found") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(resolve_missing_required_dependency_fails, CatalogFixture) {
    // 额外建一个声明 requires database 的包
    fs::create_directories(root / "leaderboard.redis" / "bin");
    std::ofstream(root / "leaderboard.redis" / "manifest.yaml") << R"({"schema_version":1,"id":"leaderboard.redis","entry":"shield_plugin_get_v1","library":{"linux":"bin/x.so"},"provides":[{"interface":"shield.leaderboard.v1"}],"requires":[{"name":"db","interface":"shield.database.v1","optional":false}],"config_schema":{"type":"object"}})";
    shield::plugin::PluginHost host; host.scan(root.string()); std::string e; host.catalog(e);
    shield::plugin::PluginConfig cfg; cfg.directory = root.string();
    shield::plugin::InstanceDecl lb; lb.id = "lb.main"; lb.package = "leaderboard.redis";
    lb.dependencies["db"] = "db.missing";  // 指向不存在的实例
    cfg.instances.push_back(lb);
    BOOST_CHECK(!host.plan_and_resolve(cfg, e));
    BOOST_TEST(e.find("plugin.dependency.missing") != std::string::npos);
}
```

- [ ] **Step 2：运行确认失败**

- [ ] **Step 3：实现 plan_and_resolve**

```cpp
// plugin_host.cpp
bool PluginHost::plan_and_resolve(const PluginConfig& cfg, std::string& error) {
    instances_.clear();
    // plan
    for (const auto& d : cfg.instances) {
        const Package* pkg = find_package(d.package);
        if (!pkg) {
            error = "plugin.package.not_found: package '" + d.package +
                    "' for instance '" + d.id + "'";
            return false;
        }
        Instance inst;
        inst.id = d.id; inst.package = pkg; inst.decl = d;
        instances_.push_back(std::move(inst));
    }
    // resolve: dependencies exist + provide interface; topo order; cycle
    for (auto& inst : instances_) {
        for (const auto& req : inst.package->manifest.requires_) {
            auto it = inst.decl.dependencies.find(req.name);
            if (it == inst.decl.dependencies.end()) {
                if (req.optional) continue;
                error = "plugin.dependency.missing: instance '" + inst.id +
                        "' missing required dependency '" + req.name + "'";
                return false;
            }
            const Instance* dep = find_instance(it->second);
            if (!dep) {
                error = "plugin.dependency.missing: instance '" + inst.id +
                        "' dependency '" + req.name + "' -> '" + it->second + "' not found";
                return false;
            }
            bool iface_ok = false;
            for (const auto& p : dep->package->manifest.provides)
                if (p.interface == req.interface) { iface_ok = true; break; }
            if (!iface_ok && !req.optional) {
                error = "plugin.dependency.missing: instance '" + dep->id +
                        "' does not provide '" + req.interface + "'";
                return false;
            }
            inst.dep_ids.push_back(dep->id);
        }
    }
    // cycle detection (Kahn)
    std::map<std::string,int> indeg;
    std::map<std::string,std::vector<std::string>> adj;
    for (const auto& i : instances_) indeg[i.id] = 0;
    for (const auto& i : instances_)
        for (const auto& d : i.dep_ids) { adj[d].push_back(i.id); indeg[i.id]++; }
    std::deque<std::string> q;
    for (const auto& [k,v] : indeg) if (v == 0) q.push_back(k);
    size_t visited = 0;
    while (!q.empty()) {
        auto n = q.front(); q.pop_front(); ++visited;
        for (const auto& m : adj[n]) if (--indeg[m] == 0) q.push_back(m);
    }
    if (visited != instances_.size()) {
        error = "plugin.dependency.cycle: circular dependency among instances";
        return false;
    }
    // order instances_ by topo (deps first)
    std::vector<Instance> ordered;
    std::map<std::string,int> remaining = indeg;
    std::deque<std::string> q2;
    for (const auto& [k,v] : remaining) if (v == 0) q2.push_back(k);
    while (!q2.empty()) {
        auto n = q2.front(); q2.pop_front();
        for (const auto& i : instances_) if (i.id == n) { ordered.push_back(i); break; }
        for (const auto& m : adj[n]) if (--remaining[m] == 0) q2.push_back(m);
    }
    instances_ = std::move(ordered);
    return true;
}
```

> 文件顶部补 `#include <deque> <map> <algorithm>`。

- [ ] **Step 4：运行测试**

Run: `ctest --test-dir build-debug -R test_plugin_host --output-on-failure`
Expected: 所有 plan/resolve 用例 PASS。

**验证检查点：** resolve 检测 package 缺失、依赖缺失、接口不符、循环依赖；拓扑排序使依赖在前。

## Task 2.3：load（dlopen + ABI 守门）

**Files:**
- Modify: `src/plugin/plugin_host.cpp`（加 load 阶段 + 把 load 接入 startup）
- Modify: `tests/plugin/test_plugin_host.cpp`

> load 需要 .so；单测里用一个最小测试插件（M2 末尾建 `tests/plugin/fixtures/minimal_test_plugin.cpp` 编成 .so）。

- [ ] **Step 1：建最小测试插件** `tests/plugin/fixtures/minimal_test_plugin.cpp`

```cpp
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"

static int minimal_create(const struct shield_plugin_create_args_v1* args,
                          struct shield_plugin_instance_v1** out,
                          struct shield_error_v1* err) {
    (void)args; (void)err;
    static struct shield_plugin_instance_v1 inst;
    inst.struct_size = sizeof(inst);
    inst.instance_id = "minimal";
    inst.get_interface = [](struct shield_plugin_instance_v1*, const char*,
                            struct shield_error_v1*) -> const void* { return nullptr; };
    inst.start = [](struct shield_plugin_instance_v1*, struct shield_error_v1*) { return 0; };
    inst.shutdown = [](struct shield_plugin_instance_v1*) {};
    *out = &inst;
    return 0;
}

extern "C" SHIELD_PLUGIN_EXPORT
const struct shield_plugin_abi_v1* shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION, sizeof(struct shield_plugin_abi_v1),
        "minimal.test", "1.0.0", minimal_create
    };
    return &abi;
}
```

- [ ] **Step 2：CMake 编译测试插件为 .so**（tests/CMakeLists.txt 追加）

```cmake
add_library(shield_minimal_test_plugin MODULE fixtures/minimal_test_plugin.cpp)
target_include_directories(shield_minimal_test_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(shield_minimal_test_plugin PROPERTIES
    PREFIX "" OUTPUT_NAME "libshield_minimal_test_plugin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test_plugins/minimal.test/bin")
file(COPY fixtures/minimal.manifest.yaml DESTINATION "${CMAKE_BINARY_DIR}/test_plugins/minimal.test")
```

`fixtures/minimal.manifest.yaml`：
```json
{"schema_version":1,"id":"minimal.test","name":"Minimal","version":"1.0.0","kind":"test","entry":"shield_plugin_get_v1","library":{"linux":"bin/libshield_minimal_test_plugin.so","macos":"bin/libshield_minimal_test_plugin.dylib","windows":"bin/shield_minimal_test_plugin.dll"},"provides":[],"requires":[],"config_schema":{"type":"object"}}
```

- [ ] **Step 3：实现 load 阶段（接入 startup 之前，先暴露给测试）**

`plugin_host.cpp` 加：
```cpp
bool PluginHost::load_all(std::string& error) {
    for (auto& inst : instances_) {
        if (inst.state != State::planned) continue;
        fs::path libpath = inst.package->root / platform_library_path(inst.package->manifest);
        std::string err;
        inst.lib = PluginLibrary::load(libpath.string(), err);
        if (!inst.lib.is_loaded()) {
            error = "plugin.entry.missing: cannot load " + libpath.string() + ": " + err;
            return false;
        }
        auto* get = reinterpret_cast<const shield_plugin_abi_v1* (*)()>(
            inst.lib.resolve(inst.package->manifest.entry.c_str()));
        if (!get) {
            error = "plugin.entry.missing: symbol '" + inst.package->manifest.entry +
                    "' not found in " + libpath.string();
            return false;
        }
        inst.abi = get();
        if (!inst.abi || inst.abi->abi_version != SHIELD_PLUGIN_ABI_VERSION) {
            error = "plugin.abi.mismatch: " + inst.id; return false;
        }
        if (inst.abi->struct_size < sizeof(shield_plugin_abi_v1)) {
            error = "plugin.abi.mismatch: struct_size too small in " + inst.id; return false;
        }
        if (!inst.abi->package_id || inst.abi->package_id != inst.package->manifest.id) {
            error = "plugin.abi.mismatch: package_id mismatch in " + inst.id; return false;
        }
        inst.state = State::loaded;
    }
    return true;
}
```

> `load_all` 声明加到 plugin_host.hpp 的 public（供测试）。

- [ ] **Step 4：测试 load + ABI 守门**

```cpp
BOOST_AUTO_TEST_CASE(load_minimal_plugin_succeeds) {
    shield::plugin::PluginHost host;
    host.scan(std::string(CMAKE_BINARY_DIR) + "/test_plugins");
    std::string e; BOOST_REQUIRE(host.catalog(e));
    shield::plugin::PluginConfig cfg; cfg.directory = std::string(CMAKE_BINARY_DIR) + "/test_plugins";
    shield::plugin::InstanceDecl in; in.id = "m"; in.package = "minimal.test"; in.required = true;
    cfg.instances.push_back(in);
    BOOST_REQUIRE(host.plan_and_resolve(cfg, e)) << e;
    BOOST_CHECK(host.load_all(e)) << e;
    BOOST_CHECK(host.find_instance("m")->state == shield::plugin::State::loaded);
}
```

> 测试需拿到 `CMAKE_BINARY_DIR`：在 CMake `target_compile_definitions(test_plugin_host PRIVATE SHIELD_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}")`，用 `SHIELD_TEST_BINARY_DIR` 宏代替。

- [ ] **Step 5：运行测试**

Run: `cmake --build build-debug --target shield_minimal_test_plugin test_plugin_host 2>&1 | tail && ctest --test-dir build-debug -R test_plugin_host --output-on-failure`
Expected: load 用例 PASS。

**验证检查点：** load 阶段 dlopen + ABI 四重守门（入口/abi_version/struct_size/package_id）。

## Task 2.4：create / start / stop + host_api_v1 实现

**Files:**
- Modify: `src/plugin/plugin_host.cpp`（create/start/stop + host_api_v1 表 + context）
- Modify: `tests/plugin/test_plugin_host.cpp`（扩展 minimal 插件验证 start）

- [ ] **Step 1：context 结构 + host_api_v1 表**

```cpp
// plugin_host.cpp 内部
struct PluginContext {  // shield_plugin_context_v1 的 host 侧实体
    PluginHost* host;
    const Instance* instance;
};

// host_api_v1 函数表（lambda）
static const shield_host_api_v1& host_api_table() {
    static shield_host_api_v1 api{};
    static bool inited = false;
    if (!inited) {
        api.log = [](shield_log_level lv, const char* pkg, const char* inst, const char* msg) {
            // 转发到 shield::log（含 pkg/inst）
            (void)lv; (void)pkg; (void)inst; (void)msg;
            // 实际：shield::log::get_logger(pkg).log(...);  M3 接入真实 logger
        };
        api.report_error = [](const shield_error_v1* err) { (void)err; };
        api.config_get = [](shield_plugin_context_v1* ctx, const char* path) -> const char* {
            auto* c = reinterpret_cast<PluginContext*>(ctx);
            if (!c) return nullptr;
            // 在 instance->decl.config 里按点路径取
            // 返回 thread_local string（host-owned）
            static thread_local std::string v;
            // 简化：仅支持顶层 key
            if (c->instance && c->instance->decl.config.contains(path))
                v = c->instance->decl.config[path].dump();
            else v.clear();
            return v.empty() ? nullptr : v.c_str();
        };
        api.dependency = [](shield_plugin_context_v1* ctx, const char* name,
                            const char* iface) -> const void* {
            auto* c = reinterpret_cast<PluginContext*>(ctx);
            if (!c || !c->host) return nullptr;
            // 查 instance.decl.dependencies[name] -> dep instance -> get_interface(iface)
            auto it = c->instance->decl.dependencies.find(name);
            if (it == c->instance->decl.dependencies.end()) return nullptr;
            const Instance* dep = c->host->find_instance(it->second);
            if (!dep || !dep->handle) return nullptr;
            shield_error_v1 e{};
            return dep->handle->get_interface(dep->handle, iface, &e);
        };
        inited = true;
    }
    return api;
}
```

> 注：`config_get` 的点路径解析 M3 增强；M2 先支持顶层 + dump。

- [ ] **Step 2：create/start/stop**

```cpp
bool PluginHost::create_all(std::string& error) {
    for (auto& inst : instances_) {
        if (inst.state != State::loaded) continue;
        PluginContext ctx{this, &inst};
        shield_plugin_create_args_v1 args{};
        args.host_api = &host_api_table();
        args.ctx = reinterpret_cast<shield_plugin_context_v1*>(&ctx);
        args.instance_id = inst.id.c_str();
        args.config_json = inst.decl.config.dump().c_str();
        shield_error_v1 e{};
        if (!inst.abi->create || inst.abi->create(&args, &inst.handle, &e) != 0 || !inst.handle) {
            error = std::string("plugin.create.failed: ") + inst.id +
                    (e.code ? (std::string(" [") + e.code + "]") : "");
            return false;
        }
    }
    return true;
}

bool PluginHost::start_all(std::string& error) {
    for (auto& inst : instances_) {  // 已拓扑序，依赖在前
        if (!inst.handle) continue;
        shield_error_v1 e{};
        if (inst.handle->start && inst.handle->start(inst.handle, &e) != 0) {
            if (inst.decl.required) {
                error = std::string("plugin.init.failed: ") + inst.id;
                inst.state = State::failed; return false;
            }
            inst.state = State::unavailable;
        } else {
            inst.state = State::started;
        }
    }
    return true;
}

void PluginHost::shutdown() {
    for (auto it = instances_.rbegin(); it != instances_.rend(); ++it) {
        if (it->handle && it->state == State::started) {
            if (it->handle->shutdown) it->handle->shutdown(it->handle);
            it->state = State::stopped;
        }
        it->handle = nullptr; it->lib = PluginLibrary{};
    }
    instances_.clear(); packages_.clear();
}
```

> `create_all`/`start_all` 加 public 声明；startup 编排见 Step 3。

- [ ] **Step 3：startup 编排**

```cpp
bool PluginHost::startup(const PluginConfig& cfg, std::string& error) {
    scan(cfg.directory);
    if (!catalog(error)) return false;
    if (!plan_and_resolve(cfg, error)) return false;
    if (!load_all(error)) return false;
    if (!create_all(error)) return false;
    if (!start_all(error)) return false;
    // 校验所有 required 实例 started
    for (const auto& i : instances_)
        if (i.decl.required && i.state != State::started) {
            error = "plugin.init.failed: required instance '" + i.id + "' not started";
            return false;
        }
    return true;
}
```

- [ ] **Step 4：扩展 minimal 插件 + 测试 start/dependency**

在 minimal_test_plugin 里：让 instance.get_interface 返回一个标记 vtable；start 记录被调用。test 验证 `find_instance("m")->state == started`。

```cpp
BOOST_AUTO_TEST_CASE(full_startup_minimal) {
    shield::plugin::PluginHost host;
    shield::plugin::PluginConfig cfg; cfg.directory = SHIELD_TEST_BINARY_DIR "/test_plugins";
    shield::plugin::InstanceDecl in; in.id = "m"; in.package = "minimal.test"; in.required = true;
    cfg.instances.push_back(in);
    std::string e;
    BOOST_REQUIRE(host.startup(cfg, e)) << e;
    BOOST_CHECK(host.find_instance("m")->state == shield::plugin::State::started);
    host.shutdown();
}
```

- [ ] **Step 5：运行测试**

Run: `ctest --test-dir build-debug -R test_plugin_host --output-on-failure`
Expected: 全部 PASS。

**验证检查点：** create→start→shutdown 完整生命周期；host_api_v1 表可用；required 守门。

## Task 2.5：get_by_binding + global_host

**Files:**
- Modify: `src/plugin/plugin_host.cpp`（get_binding_vtable + global_host）
- Modify: `tests/plugin/test_plugin_host.cpp`

- [ ] **Step 1：实现**

```cpp
const void* PluginHost::get_binding_vtable(std::string_view binding, const char* iface) const {
    // bindings_ 存于 impl_：这里简化为线性查
    for (const auto& b : bindings_)  // 需在 startup 时从 cfg 拷入
        if (b.logical == binding) {
            const Instance* inst = find_instance(b.instance_id);
            if (!inst || !inst->handle || inst->state != State::started) return nullptr;
            shield_error_v1 e{};
            return inst->handle->get_interface(inst->handle, iface, &e);
        }
    return nullptr;
}
```

> 需在 plugin_host.hpp 的 Impl 存 `std::vector<BindingDecl> bindings_`，startup 时 `impl_->bindings_ = cfg.bindings;`；声明 `const void* get_binding_vtable(std::string_view, const char*) const;`。

```cpp
PluginHost& global_host() {
    static PluginHost h;
    return h;
}
```

- [ ] **Step 2：测试 get_by_binding**

让 minimal 插件 provide 一个接口（修 manifest + get_interface 返回标记）。验证 `get_by_binding<TestIface>("test.default")` 返回非空。

- [ ] **Step 3：运行测试**

Run: `ctest --test-dir build-debug -R test_plugin_host --output-on-failure`
Expected: PASS。

**验证检查点：** binding 名 → typed vtable 查询通路打通（纵切关键）。

---

# M3 — 纵切端到端：sqlite → DatabasePool → bootstrap

> 这是整个重构的关键验收里程碑。

## Task 3.1：迁移 sqlite 插件到新 ABI

**Files:**
- Modify: `plugins/sqlite/shield_db_sqlite.cpp`
- Create: `plugins/sqlite/manifest.yaml`
- Modify: `plugins/sqlite/CMakeLists.txt`

- [ ] **Step 1：写 manifest.yaml**

`plugins/sqlite/manifest.yaml`：
```json
{
  "schema_version": 1,
  "id": "database.sqlite",
  "name": "SQLite",
  "version": "1.0.0",
  "kind": "database",
  "description": "SQLite provider for shield.database.v1",
  "entry": "shield_plugin_get_v1",
  "library": {
    "windows": "bin/shield_database_sqlite.dll",
    "linux": "bin/libshield_database_sqlite.so",
    "macos": "bin/libshield_database_sqlite.dylib"
  },
  "provides": [{"interface": "shield.database.v1", "capabilities": ["sql","transactions"]}],
  "requires": [],
  "config_schema": {
    "type": "object",
    "properties": {
      "database": {"type": "string", "default": ":memory:"},
      "query_timeout_ms": {"type": "integer", "default": 5000, "minimum": 1, "maximum": 300000}
    }
  }
}
```

- [ ] **Step 2：改造 shield_db_sqlite.cpp**

保留全部 sqlite 实现逻辑（`run_prepared`/`clear_result`/`dup_*` 等不变）。改动：
1. `#include "shield/plugin/database.h"`（替代 db_plugin.h + plugin.h）。
2. 把 `shield_db_plugin_api()` 内的静态表类型从 `shield_db_plugin` 改为 `shield_database_v1`，`SHIELD_DB_ABI_VERSION` 字段改为 `sizeof(shield_database_v1)`。
3. 删除旧 `g_plugin`（shield_plugin）与 `shield_plugin_api()` 包装。
4. 新增 `shield_plugin_abi_v1` + `create`：

```cpp
// 每个 instance 一个 sqlite handle wrapper
struct sqlite_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    std::string db_path;
    // shield_database_v1 是无状态函数表，所有 instance 共享一份
};

static const shield_database_v1& db_vtable();  // 原 shield_db_plugin_api 的静态表，改类型名

static int sqlite_create(const shield_plugin_create_args_v1* args,
                         shield_plugin_instance_v1** out, shield_error_v1* err) {
    (void)err;
    auto* inst = new sqlite_instance{};
    inst->instance_id = args->instance_id ? args->instance_id : "";
    // 解析 config_json 拿 database 路径（nlohmann）
    inst->db_path = ":memory:";
    if (args->config_json) {
        try { auto j = nlohmann::json::parse(args->config_json);
              if (j.contains("database")) inst->db_path = j["database"].get<std::string>();
        } catch (...) {}
    }
    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1* self, const char* iface,
                                   shield_error_v1*) -> const void* {
        if (std::string(iface) == SHIELD_DATABASE_INTERFACE)
            return &db_vtable();  // self 可携带 per-instance 状态
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) { return 0; };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        delete static_cast<sqlite_instance*>(self);  // 注意：get_interface 捕获问题见下
    };
    *out = &inst->shell;
    return 0;
}
```

> ⚠️ shutdown 内 `delete self`，但 get_interface 是无捕获 lambda 转 C 函数指针——不能捕获 per-instance。**修正**：把 db_path 等状态放进 `shield_db_conn`（connect 时从 args->database 取），instance 本身无状态。shutdown 只 `delete static_cast<sqlite_instance*>(self)`。get_interface 返回共享静态 vtable，connect 时由调用方（DatabasePool）传 database 路径。这样最干净，且复用现有 connect 签名（database 来自 connect_args，不是 instance config）。

**最终采用**：instance 无状态；config_json 仅作校验；真实连接参数由 DatabasePool 通过 `shield_db_connect_args.database` 传入（与现状一致）。sqlite_create 里只装 shell。

```cpp
extern "C" SHIELD_PLUGIN_EXPORT
const shield_plugin_abi_v1* shield_plugin_get_v1(void) {
    static const shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION, sizeof(shield_plugin_abi_v1),
        "database.sqlite", "1.0.0", sqlite_create
    };
    return &abi;
}
```

- [ ] **Step 3：CMake 改产物名与布局** `plugins/sqlite/CMakeLists.txt`

```cmake
add_library(shield_database_sqlite MODULE shield_db_sqlite.cpp)
target_include_directories(shield_database_sqlite PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/plugins/sqlite)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
target_link_libraries(shield_database_sqlite PRIVATE unofficial::sqlite3::sqlite3 nlohmann_json::nlohmann_json)
set_target_properties(shield_database_sqlite PROPERTIES
    PREFIX "" OUTPUT_NAME "libshield_database_sqlite"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/database.sqlite/bin")
file(COPY manifest.yaml DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/database.sqlite")
message(STATUS "  + database.sqlite: SQLite provider (v1 ABI)")
```

> 删除旧 smoke_test 目标（依赖旧 shield_db_plugin_api，已废）。保留 `shield_db_sqlite_smoke` 则需同步迁移，M3 暂删，端到端测试替代。

- [ ] **Step 4：验证插件编译**

Run: `cmake --build build-debug --target shield_database_sqlite 2>&1 | tail`
Expected: 编译通过，产出 `plugins/database.sqlite/bin/libshield_database_sqlite.so`。

**验证检查点：** sqlite 插件导出 `shield_plugin_get_v1`，产出符合目录布局。

## Task 3.2：DatabasePool 改走 binding

**Files:**
- Modify: `src/data/data.cpp:339-489`（initialize）

- [ ] **Step 1：改造 initialize**

替换 `data.cpp:412-489` 的「Plugin discovery」整段为：

```cpp
// 从 PluginHost 拿 database binding
auto* db = shield::plugin::global_host()
               .get_by_binding<shield_database_v1>("database.default");
if (db) {
    impl_->plugin = db;   // const shield_database_v1*（签名同旧 shield_db_plugin*）
    impl_->driver_name = db->name ? db->name : "plugin";
    // 连接参数从 binding instance 的 config 读（host/port/database/...）
    auto* inst = shield::plugin::global_host().find_instance(
        shield::plugin::global_host().binding_instance_id("database.default"));
    // ... 用 inst->decl.config 填 connect_args（M3 简化：sqlite 用 database 路径）
    SHIELD_LOG_INFO(log, "Database via plugin binding 'database.default'");
} else {
    // 降级 mock（保留现有 mock 行为）：binding 不存在 → mock pool
    impl_->mock = true;
    // ... 原 mock 分支
}
```

> 需在 PluginHost 加 `std::string binding_instance_id(std::string_view) const;`。
> `PluginDatabaseConnection`（data.cpp:165）仅把成员类型 `const shield_db_plugin*` → `const shield_database_v1*`，所有 `plugin_->query/...` 调用不变（签名一致）。

- [ ] **Step 2：include 改动**

`data.cpp` 顶部：`#include "shield/plugin/database.h"` + `#include "shield/plugin/plugin_host.hpp"`，删除 `#include "shield/data/db_plugin.h"`（或 `shield/plugin/db_plugin.h`，按实际）。`data.hpp`/data.cpp 内 `shield_db_plugin` 类型名全局替换为 `shield_database_v1`。

- [ ] **Step 3：保留 mock 降级**

确保 `database.default` binding 不存在时，DatabasePool 仍走 mock（CI 无 sqlite 时 `test_data_pool` 继续通过）。

- [ ] **Step 4：编译 + 回归**

Run: `cmake --build build-debug --target shield_data test_data_pool 2>&1 | tail && ctest --test-dir build-debug -R test_data_pool --output-on-failure`
Expected: shield_data 编译通过；test_data_pool PASS（走 mock 分支）。

**验证检查点：** DatabasePool 不再自 dlopen；binding 存在时走插件，不存在时降级 mock；旧 pool 测试不回归。

## Task 3.3：bootstrap 集成 PluginHost

**Files:**
- Modify: `src/bootstrap/bootstrap.cpp`（:224 data_init 之前 + shutdown 段）

- [ ] **Step 1：startup 注入**

在 `bootstrap.cpp` 的 `data_init`（:224）之前：

```cpp
{
    auto pc = shield::plugin::load_plugin_config();
    std::string perr;
    if (!shield::plugin::global_host().startup(pc, perr)) {
        SHIELD_LOG_ERROR(shield::log::get_logger("bootstrap"),
                         "Plugin startup failed: " + perr);
        return false;  // bootstrap 失败
    }
}
```

- [ ] **Step 2：shutdown 调用**

在 shutdown 流程（data_close 之后）：

```cpp
shield::plugin::global_host().shutdown();
```

- [ ] **Step 3：app.yaml 加 plugins 段**（开发/测试：sqlite 内存库）

`config/app.yaml` 追加：
```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: db.main
      package: database.sqlite
      required: false
      config:
        database: ":memory:"
  bindings:
    database.default: db.main
```

> `required: false`：没有插件 .so 时不阻塞启动（降级 mock）。

- [ ] **Step 4：编译 bootstrap + 冒烟**

Run: `cmake --build build-debug --target shield_bootstrap 2>&1 | tail`
Expected: 编译通过。

**验证检查点：** bootstrap 启动时加载插件、关闭时卸载。

## Task 3.4：端到端集成测试

**Files:**
- Create: `tests/plugin/test_plugin_e2e.cpp`

- [ ] **Step 1：测试**（startup PluginHost + sqlite binding → DatabasePool 建表/查询/事务）

```cpp
#define BOOST_TEST_MODULE shield_plugin_e2e
#include <boost/test/included/unit_test.hpp>
#include "shield/plugin/plugin_host.hpp"
#include "shield/plugin/database.h"
#include "shield/data/data.hpp"

BOOST_AUTO_TEST_CASE(sqlite_e2e_through_binding) {
    using namespace shield::plugin;
    PluginConfig cfg;
    cfg.directory = SHIELD_PLUGINS_DIR;  // CMake 注入：指向 build output 的 plugins/
    InstanceDecl in; in.id = "db.main"; in.package = "database.sqlite"; in.required = true;
    in.config = nlohmann::json{{"database", ":memory:"}};
    cfg.instances.push_back(in);
    BindingDecl b; b.logical = "database.default"; b.instance_id = "db.main";
    cfg.bindings.push_back(b);

    PluginHost host; std::string e;
    BOOST_REQUIRE(host.startup(cfg, e)) << e;
    auto* db = host.get_by_binding<shield_database_v1>("database.default");
    BOOST_REQUIRE(db != nullptr);

    // connect + create table + insert + query
    shield_db_connect_args args{}; args.database = ":memory:"; args.query_timeout_ms = 5000;
    char err[256] = {0};
    auto* conn = db->connect(&args, err, sizeof(err));
    BOOST_REQUIRE(conn != nullptr);
    shield_db_result r{};
    BOOST_CHECK_EQUAL(db->execute(conn, "CREATE TABLE t(id INTEGER)", nullptr, 0, &r), 0);
    BOOST_CHECK_EQUAL(db->execute(conn, "INSERT INTO t VALUES(1)", nullptr, 0, &r), 0);
    BOOST_CHECK_EQUAL(db->query(conn, "SELECT id FROM t", nullptr, 0, &r), 0);
    BOOST_CHECK_EQUAL(r.row_count, 1);
    db->free_result(&r);
    db->disconnect(conn);
    host.shutdown();
}
```

- [ ] **Step 2：CMake 注册 + 注入路径**

```cmake
add_executable(test_plugin_e2e plugin/test_plugin_e2e.cpp)
target_link_libraries(test_plugin_e2e PRIVATE shield_plugin shield_data
    nlohmann_json::nlohmann_json Boost::unit_test_framework Boost::headers)
target_include_directories(test_plugin_e2e PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src/plugin)
target_compile_definitions(test_plugin_e2e PRIVATE
    SHIELD_PLUGINS_DIR="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins")
add_test(NAME test_plugin_e2e COMMAND test_plugin_e2e)
set_tests_properties(test_plugin_e2e PROPERTIES LABELS "plugin" TIMEOUT 30)
shield_copy_runtime_dlls(test_plugin_e2e)
```

- [ ] **Step 3：运行**

Run: `cmake --build build-debug --target shield_database_sqlite test_plugin_e2e 2>&1 | tail && ctest --test-dir build-debug -R test_plugin_e2e --output-on-failure`
Expected: sqlite 建表/插入/查询/断开全 PASS。

**验证检查点（M3 里程碑）：** ABI 设计端到端验证通过——manifest→catalog→pipeline→binding→DatabasePool→sqlite 查询全链路跑通。

## Task 3.5：回归确认

- [ ] **Step 1：跑全部现有 ctest**

Run: `ctest --test-dir build-debug --output-on-failure -j4`
Expected: 现有测试全 PASS（test_data_pool / test_hello_world_acceptance / test_transport_frame 等）。

- [ ] **Step 2：删除旧 plugin.h / db_plugin.h 的时机判断**

此时 sqlite 已迁完，但 mysql/pg/redis/auth/metrics/health/matchmaking 仍 include 旧头。**保留旧头到 M5/M6 迁移完再删。** 若旧头与新头有符号冲突（如 `SHIELD_PLUGIN_EXPORT` 重名），在新头 `#include` 旧头或用 include guard 隔离。

**验证检查点：** 无回归；旧头安全保留待 M5/M6。

---

# M4 — Lua Introspection

## Task 4.1：重写 register_plugin_api

**Files:**
- Modify: `src/lua/lua_api.cpp:2185-2224`
- Test: `tests/plugin/test_lua_plugin_api.cpp`

- [ ] **Step 1：PluginHost 加 introspection（packages/instances/binding）**

`plugin_host.hpp` 补：
```cpp
struct PackageInfo { std::string id, version, kind; std::vector<std::string> provides; };
struct InstanceInfo { std::string id, package, state; bool required; };
struct BindingInfo { std::string logical, instance_id, interface; };

std::vector<PackageInfo> list_packages() const;
std::vector<InstanceInfo> list_instances() const;
std::optional<BindingInfo> get_binding(std::string_view name) const;
```

`plugin_host.cpp` 实现上述（遍历 packages_/instances_/bindings_，state 转 string）。

- [ ] **Step 2：重写 lua_api.cpp:2185**

```cpp
void register_plugin_api(sol::table& shield) {
    sol::state_view lua(shield.lua_state());
    auto plugin = lua.create_table();

    plugin.set_function("packages", [](sol::this_state s) -> sol::table {
        sol::state_view lua(s);
        auto t = lua.create_table();
        for (const auto& p : shield::plugin::global_host().list_packages()) {
            auto row = lua.create_table();
            row["id"] = p.id; row["version"] = p.version; row["kind"] = p.kind;
            auto prov = lua.create_table();
            for (size_t i = 0; i < p.provides.size(); ++i) prov[i+1] = p.provides[i];
            row["provides"] = prov;
            t[t.size() + 1] = row;
        }
        return t;
    });

    plugin.set_function("instances", [](sol::this_state s) -> sol::table {
        sol::state_view lua(s);
        auto t = lua.create_table();
        for (const auto& i : shield::plugin::global_host().list_instances()) {
            auto row = lua.create_table();
            row["id"] = i.id; row["package"] = i.package;
            row["state"] = i.state; row["required"] = i.required;
            t[t.size() + 1] = row;
        }
        return t;
    });

    plugin.set_function("instance", [](std::string id) -> sol::object {
        // 返回单实例表或 nil
        for (const auto& i : shield::plugin::global_host().list_instances())
            if (i.id == id) { /* 返回 row */ }
        return sol::nil;
    });

    plugin.set_function("binding", [](std::string name) -> sol::object {
        auto b = shield::plugin::global_host().get_binding(name);
        if (!b) return sol::nil;
        // 返回 {instance_id=, interface=}
        return sol::nil;  // 实际构造 table
    });

    shield["plugin"] = plugin;
}
```

- [ ] **Step 3：测试**（启动 minimal 插件后 Lua 查询返回正确）

- [ ] **Step 4：注册 + 运行**

Run: `ctest --test-dir build-debug -R test_lua_plugin_api --output-on-failure`
Expected: PASS。

**验证检查点：** Lua packages/instances/instance/binding 返回真实 catalog/runtime 数据。

---

# M5 — 横向插件迁移（recipe + 差异表）

## Task 5.1：迁移 recipe（基于 M3 sqlite 模式）

每个插件迁移遵循统一步骤：
1. 写 `<plugin>/manifest.yaml`（id/kind/library/provides/config_schema）。
2. 改 `.cpp`：
   - `#include` 换为新 interface 头（database.h/auth.h/metrics.h/health.h）+ abi.h。
   - 旧静态 vtable 类型 → 新 interface v1 类型，`abi_version` → `struct_size`。
   - 删除旧 `shield_plugin` + `shield_plugin_api()`。
   - 加 `shield_plugin_abi_v1` + `create()`（装 instance shell，get_interface 返回静态 vtable）。
3. 改 `CMakeLists.txt`：target 名 → `shield_<package>`，MODULE，输出到 `plugins/<id>/bin/`，`file(COPY manifest.yaml ...)`。
4. 删除对应旧 `include/shield/plugin/<x>_plugin.h`（被新 interface 头取代）。

## Task 5.2-5.7：各插件差异表

| 插件 | 新 id | interface 头 | provides interface | kind | 备注 |
|---|---|---|---|---|---|
| mysql | database.mysql | database.h | shield.database.v1 | database | 复用 mysql-connector；connect_args 同 sqlite |
| postgresql | database.postgresql | database.h | shield.database.v1 | database | libpq |
| auth_jwt | auth.jwt | auth.h | shield.auth.v1 | auth | 新建 auth.h（verify/issue 等，按现有 auth_plugin.h 签名演化） |
| metric_prometheus | metrics.prometheus | metrics.h | shield.metrics.v1 | metrics | 新建 metrics.h |
| health_http | health.http | health.h | shield.health.v1 | health | 新建 health.h |
| matchmaking_elo | matchmaking.elo |（matchmaking.h 占位） | shield.matchmaking.v1 | matchmaking | 非首批 7 接口，仅迁移入口 |

> 每个插件按 recipe 执行，配 smoke test（直接 dlopen + get_interface 验证）。新 interface 头（auth/metrics/health/matchmaking）从对应旧 `*_plugin.h` 演化签名（同 database.h 模式）。

**Task 5.x 每个插件验收：** `cmake --build --target shield_<package>` 通过 + smoke test PASS。

**验证检查点：** 7 个首批 interface 全部有 provider；旧 *_plugin.h 全部删除。

---

# M6 — redis 三件套重构

## Task 6.1：redis 连接 helper

**Files:**
- Create: `plugins/redis/redis_client.hpp`（内部头，非导出）

封装 redis-plus-plus 连接（`sw::redis::Redis`，参考 `src/data/data.cpp:725` 的 `RealRedisConnection` 与旧 `shield_redis.cpp`）。提供 connect/disconnect/get/set/hget/del/incr/zadd/zrange 等。hiredis 由 redis-plus-plus 自动带入，不显式依赖。

- [ ] **Step 1：读旧 shield_redis.cpp + redis_plugin.h，抽取 `sw::redis::*` 连接与命令逻辑到 redis_client.hpp。**

## Task 6.2-6.3：cache.redis / queue.redis / leaderboard.redis 内嵌连接

- [ ] **Step 1：cache_redis.cpp** 改为：include redis_client.hpp；start() 内读 config（host/port/password）建连；废除 `find_plugin("shield_redis")`。导出 shield_cache_v1。
- [ ] **Step 2：queue_redis.cpp / leaderboard_redis.cpp** 同理。
- [ ] **Step 3：** 各写 manifest.yaml（id=cache.redis/queue.redis/leaderboard.redis，requires=[]，config_schema 含 host/port/password）。

## Task 6.4：删除 shield_redis.cpp

- [ ] **Step 1：** 删除 `plugins/redis/shield_redis.cpp` + `include/shield/plugin/redis_plugin.h`。
- [ ] **Step 2：** 更新 `plugins/redis/CMakeLists.txt`：从单 target 拆为三个独立 MODULE（cache/queue/leaderboard），各自链接 hiredis。
- [ ] **Step 3：** smoke test 每个三件套（需本地 redis 或 skip）。

**验证检查点：** redis 不再作为公共插件类型；三件套各自独立、显式依赖连接配置。

---

# 最终清理

- [ ] 删除 `include/shield/plugin/plugin.h`（确认无 include 残留）。
- [ ] 全量 ctest 回归：`ctest --test-dir build-debug -j4 --output-on-failure`。
- [ ] 更新 `docs/plugin-system.md` 的 Open Decisions（记录本次结论）。

---

## Self-Review（plan 对 spec 的覆盖核对）

- **ABI 头族（spec §1.1）** → Task 0.1-0.3 ✓
- **interface vtable 复用（§1.2）** → Task 0.3, 3.1 ✓
- **C++ 数据模型（§1.3）** → Task 1.2, 2.1 ✓
- **七阶段 pipeline（§2.1）** → Task 2.1-2.4 ✓
- **PluginHost（§2.2）** → Task 2.1-2.5 ✓
- **required 语义（§2.3）** → Task 2.4 start_all ✓
- **host_api_v1 + dependency（§3.1）** → Task 2.4 ✓
- **plugins 段配置（§3.2）** → Task 1.4 ✓
- **schema 校验（§3.3）** → Task 1.3 ✓
- **错误模型（§3.4）** → Task 2.1-2.4 各 phase code ✓
- **DatabasePool 迁移（§4.1）** → Task 3.2 ✓
- **8 插件迁移（§4.2）** → Task 3.1 + 5.2-5.7 + 6.2-6.3 ✓
- **redis 重构（§4.3）** → Task 6.x ✓
- **Lua introspection（§4.4）** → Task 4.1 ✓
- **CMake 构建（§4.5）** → Task 0.4 + 各插件 CMake ✓
- **bootstrap 集成（§4.6）** → Task 3.3 ✓
- **删除清单（§5.3）** → Task 0.4, 3.5, 5.x, 6.4, 最终清理 ✓
- **里程碑 M0-M6（§5.1）** → 各 M 段对应 ✓
- **测试策略（§5.2）** → TDD 贯穿每任务 ✓

**类型一致性核对**：`shield_database_v1`、`PluginHost::get_by_binding<T>`、`global_host()`、`Instance::state`、`PluginContext` 在跨任务中签名一致。✓
