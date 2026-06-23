# Shield 插件系统 v1 重构设计

- **日期**：2026-06-22
- **目标**：按 `docs/plugin-system.md`（Shield Plugin System v1）完全重构现有插件系统。
- **状态**：设计已定稿，待实现。
- **权威文档**：`docs/plugin-system.md`（本文档是其实现落地说明，冲突时以 `plugin-system.md` 为准）。
- **现行实现说明**：当前运行时与正式文档已收敛到单一 manifest 来源，只支持 `manifest.yaml`。下文若仍出现旧 manifest 文件名或旧表述，均属于历史设计语境，应以当前 `docs/plugin-system.md` 为准。

---

## 0. 背景与已确认决策

### 0.1 现状（已核实）

当前存在**两套割裂**的插件机制：

1. **`PluginManager`（`src/core/plugin_manager.cpp`）—— 死代码**。全仓无业务调用方，`bootstrap` 未集成，仅 `lua_api.cpp` 有 4 处 TODO 注释。其 ABI 正是文档要废弃的旧设计：
   - 入口 `shield_plugin_api()`，返回 `shield_plugin`（含 `enum shield_plugin_type` 13 枚举）。
   - 全局 `find_plugin()` + 裸 `get_plugin_vtable()`。
   - 配置是 YAML key-value；错误是裸 string；从文件名 `find("db")` 猜类型。

2. **`DatabasePool`（`src/data/data.cpp`）—— 活代码**。自己 `dlopen` 数据库插件，调用 `shield_db_plugin_api()`（`include/shield/plugin/db_plugin.h:160`），**绕过 PluginManager**。`bootstrap.cpp:224` 调 `shield::data::database().initialize()`。

### 0.2 已确认决策

| 决策 | 结论 |
|---|---|
| 重构范围 | **完全替换**：新 v1 骨架 + 迁移 8 插件 + 废除旧 ABI + DatabasePool 改走 binding + Lua 全实现 + redis 三件套改依赖注入。 |
| 配置格式 | **外部 YAML，内部 JSON-compatible 值模型，主配置不动**。插件 manifest 固定为 `manifest.yaml`；`plugins.instances/bindings/config` 作为 YAML 子树嵌在 `app.yaml`，进入插件系统后按 JSON-compatible 值模型处理。不迁移 8 个 profile，不动 `Config` 子系统，不动 `database.toml`。 |
| 实施编排 | **方案 A：纵切端到端 + TDD**。先打通 database.v1 最小垂直链路验证 ABI，再横向铺开。 |
| 接口集合 | 文档列的 7 个：`shield.database/cache/queue/leaderboard/auth/metrics/health.v1`。 |
| schema 校验 | 自研最小子集，不引新依赖（已有 nlohmann-json）。 |
| redis 基础设施插件 | 废除 `shield_redis.cpp` 公共插件；cache/queue/leaderboard 三件套各自内嵌 redis 客户端。 |
| DatabasePool 迁移 | 不再自 dlopen；从 binding `database.default` 拿 `shield_database_v1`，连接池逻辑保留在 data 层。 |
| C++ typed 访问 | host 提供 `PluginHost::get_by_binding<T>(binding)` C++ wrapper。 |
| 旧符号处置 | 删除 `PluginManager`、`shield_plugin_api`、`shield_db_plugin_api`、`enum shield_plugin_type`，不留兼容层。 |
| 测试 | 沿用 CTest + 手写可执行，不引框架。 |
| 构建 | 每 plugin 目录产出 `plugins/<id>/{manifest.yaml, bin/libshield_<id>.so}`。 |

---

## 1. C ABI 与核心数据模型

### 1.1 新 ABI 头文件族（`include/shield/plugin/`）

替换现有 `plugin.h`。职责单一，分三类：

```
include/shield/plugin/
  abi.h          # shield_plugin_abi_v1 / create_args / instance_v1 / entry
  host_api.h     # shield_host_api_v1 / context / error_v1 / log_level
  database.h     # shield_database_v1 + connect_args/conn/result
  cache.h        # shield_cache_v1
  queue.h        # shield_queue_v1
  leaderboard.h  # shield_leaderboard_v1
  auth.h         # shield_auth_v1
  metrics.h      # shield_metrics_v1
  health.h       # shield_health_v1
```

**核心结构**（`abi.h`）：

```c
#define SHIELD_PLUGIN_ABI_VERSION 1

// 结构化错误，贯穿所有 phase
typedef struct shield_error_v1 {
    const char* code;         // "plugin.abi.mismatch"
    const char* message;
    const char* hint;
    const char* package_id;
    const char* instance_id;
    const char* phase;        // "scan"|"catalog"|"plan"|"resolve"|"load"|"create"|"start"
} shield_error_v1;

// Opaque context：host 分配，插件用它回调 host（查依赖/配置）
typedef struct shield_plugin_context_v1 shield_plugin_context_v1;

// 插件实例统一外壳：host 只认这三个方法，不关心内部
typedef struct shield_plugin_instance_v1 {
    uint32_t struct_size;
    const char* instance_id;
    // 查询此实例提供的某 interface vtable（host 已校验过 manifest.provides）
    const void* (*get_interface)(struct shield_plugin_instance_v1* self,
                                 const char* interface_name,
                                 shield_error_v1* err);
    int  (*start)(struct shield_plugin_instance_v1* self, shield_error_v1* err);
    void (*shutdown)(struct shield_plugin_instance_v1* self);
} shield_plugin_instance_v1;

typedef struct shield_plugin_create_args_v1 {
    const struct shield_host_api_v1* host_api;
    shield_plugin_context_v1* ctx;
    const char* instance_id;
    const char* config_json;   // 校验后的实例配置（JSON 字符串）
} shield_plugin_create_args_v1;

// 每个 .so 唯一入口
typedef struct shield_plugin_abi_v1 {
    uint32_t abi_version;      // == 1
    uint32_t struct_size;      // host 校验 >= sizeof(最小)
    const char* package_id;    // 必须与 manifest.yaml id 一致
    const char* package_version;
    int (*create)(const shield_plugin_create_args_v1* args,
                  shield_plugin_instance_v1** out,
                  shield_error_v1* err);
} shield_plugin_abi_v1;

SHIELD_PLUGIN_EXPORT
const shield_plugin_abi_v1* shield_plugin_get_v1(void);
```

**设计要点**：
- `instance_v1` 是统一外壳，`get_interface` 按 interface name 取 vtable —— **interface name 取代 `enum shield_plugin_type`**。
- `create`（构造 handle）与 `start`（真正 init）分离，对齐文档 pipeline 的 create→start 两阶段。
- `struct_size` + `abi_version` + `package_id` 三重守门。

### 1.2 interface vtable 复用现有签名（DRY）

`shield_database_v1` 直接继承现有 `shield_db_plugin`（`db_plugin.h:94-151`）的函数签名，仅改类型名与 `abi_version→struct_size`：

```c
// database.h
#define SHIELD_DATABASE_INTERFACE "shield.database.v1"
typedef struct shield_database_v1 {
    uint32_t struct_size;
    // connect/disconnect/ping/query/execute/begin/commit/rollback/free_result
    // 签名与现有 shield_db_plugin 完全一致，复用 shield_db_conn/shield_db_result/shield_db_connect_args
} shield_database_v1;
```

**价值**：`PluginDatabaseConnection`（`data.cpp:165`）几乎零改动，仅把 `const shield_db_plugin*` 换成 `const shield_database_v1*`。DatabasePool 迁移成本最小化。

其余 6 个 interface（cache/queue/leaderboard/auth/metrics/health）同理从现有 `*_plugin.h` 演化。`shield_db_conn/shield_db_result/shield_db_connect_args` 这些公共结构搬到 `database.h`，旧 `db_plugin.h` 删除。

### 1.3 C++ 数据模型（`src/plugin/`）

新建 `src/plugin/` 目录（从 `src/core/` 拆出，单一职责）：

```cpp
namespace shield::plugin {

struct Manifest {              // ← manifest.yaml 反序列化
    int schema_version = 1;
    std::string id, name, version, kind, description, entry;
    struct Lib { std::string windows, linux, macos; } library;
    struct Provide { std::string interface; std::vector<std::string> capabilities; };
    std::vector<Provide> provides;
    struct Require { std::string name, interface; bool optional = false; };
    std::vector<Require> requires_;
    nlohmann::json config_schema;
    // 从 JSON 反序列化（nlohmann ADL）
};

struct InstanceDecl {          // ← app.yaml plugins.instances[]
    std::string id, package;
    bool required = true;
    std::map<std::string, std::string> dependencies;  // require name -> instance id
    nlohmann::json config;
};

struct BindingDecl { std::string logical; std::string instance_id; };  // database.default -> db.main

enum class State { planned, loaded, started, unavailable, failed, stopped };

struct Package {               // catalog 条目
    Manifest manifest;
    std::filesystem::path root;
};

struct Instance {              // 运行期
    std::string id;
    const Package* package = nullptr;
    InstanceDecl decl;
    State state = State::planned;
    shield_plugin_abi_v1 const* abi = nullptr;
    shield_plugin_instance_v1* handle = nullptr;
    PluginLibrary lib;
    std::vector<std::string> dep_ids;   // 拓扑解析后的依赖 instance id
};
}
```

`PluginLibrary`（跨平台 dlopen 封装）从现有 `plugin_manager.cpp:23-85` 搬入 `src/plugin/plugin_library.cpp`，实现已验证可用。

---

## 2. Pipeline 与 PluginHost

### 2.1 七阶段流水线（严格对齐文档）

```
scan → catalog → plan → resolve → load → create → start
                                                                 stop ← shutdown
```

| 阶段 | 输入 | 动作 | 失败 code |
|---|---|---|---|
| `scan` | `plugins.directory` | 遍历 `*/manifest.yaml`，只读 YAML manifest 不加载代码 | `plugin.scan.failed` |
| `catalog` | scan 结果 | 校验 manifest schema、id 唯一、平台库路径存在、interface 声明合法 | `plugin.manifest.invalid` |
| `plan` | catalog + instances | 按 `plugins.instances` 建启动计划，关联 package | `plugin.package.not_found` |
| `resolve` | plan | 校验配置合法（config_schema）、依赖可满足、binding 指向合法、拓扑排序查循环 | `plugin.config.invalid` / `plugin.dependency.missing` / `plugin.dependency.cycle` |
| `load` | 拓扑序 | `dlopen` + 解析 `shield_plugin_get_v1`，校验 abi_version/struct_size/package_id 一致 | `plugin.entry.missing` / `plugin.abi.mismatch` |
| `create` | load 后 | 调 `create()`，传 host_api + ctx + config_json，得 instance handle | `plugin.create.failed` |
| `start` | 拓扑序（依赖先） | 调 `instance->start()`，失败按 required 决定 | `plugin.init.failed` |
| `stop` | shutdown | 拓扑逆序调 `shutdown()` | — |

**依赖拓扑**：`resolve` 阶段对 instance 依赖图做 Kahn 拓扑排序，环→`plugin.dependency.cycle`。`start` 按拓扑序，`stop` 按逆序。

### 2.2 PluginHost（替换 PluginManager）

```cpp
namespace shield::plugin {

struct PluginConfig {                       // ← app.yaml plugins 段
    std::string directory = "./plugins";
    std::vector<InstanceDecl> instances;
    std::vector<BindingDecl> bindings;
};

class PluginHost {
public:
    bool startup(const PluginConfig& cfg, std::string& error);  // 跑完整 pipeline
    void shutdown();                                            // stop 全部，逆序
    ~PluginHost();

    // 业务代码入口：binding 名 → typed interface vtable
    template<typename Interface>
    const Interface* get_by_binding(std::string_view binding_name) const;

    // 按依赖名取（供插件内部 ctx 用）
    const void* dependency(const shield_plugin_context_v1* ctx,
                           std::string_view name,
                           std::string_view interface_name) const;

    // introspection（Lua 用）
    std::vector<PackageInfo> packages() const;
    std::vector<InstanceInfo> instances() const;
    std::optional<InstanceInfo> instance(std::string_view id) const;
    std::optional<BindingInfo> binding(std::string_view name) const;
};

PluginHost& global_host();  // 进程级单例，bootstrap 持有
}
```

**`get_by_binding<T>` 实现**：查 binding 名→instance id→`instance->handle->get_interface(T::interface_name)`→`static_cast<const T*>`。类型安全，业务代码不碰裸 vtable。

### 2.3 required 语义

- `required:true` 任一 phase 失败 → 整个 `startup` 失败，进程启动失败。
- `required:false` 失败 → 该 instance 置 `unavailable`，记录结构化错误，依赖它的 required instance 连锁失败。

---

## 3. Host API、依赖注入、配置、错误

### 3.1 shield_host_api_v1（`host_api.h`）

```c
typedef struct shield_host_api_v1 {
    void (*log)(shield_log_level level, const char* plugin_id,
                const char* instance_id, const char* message);
    void (*report_error)(const shield_error_v1* err);
    const char* (*config_get)(shield_plugin_context_v1* ctx, const char* path);  // 点分路径
    const void* (*dependency)(shield_plugin_context_v1* ctx,
                              const char* name, const char* interface_name);
} shield_host_api_v1;
```

- `dependency(ctx, name, iface)`：host 校验 name 在该 instance 的 manifest `requires` 内，且目标 instance 提供 iface，返回其 vtable 指针；否则 NULL。**禁止跨插件任意查找**（对齐文档 no global registry）。
- `config_get(ctx, path)`：从该实例的校验后配置取值，点分路径，返回 JSON 片段字符串。

### 3.2 C++ 配置：plugins 段（app.yaml 子树）

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: db.main
      package: database.sqlite
      required: true
      config:
        database: ":memory:"           # sqlite 用内存库，CI 可跑
    - id: db.audit
      package: database.mysql
      required: false
      config: { host: 127.0.0.1, port: 3306, database: shield, username: root, password: secret }
  bindings:
    database.default: db.main
```

`Config` 子系统不动；`PluginConfig` 从 `global_config()` 读 `plugins.*` 子树（YAML→JSON 超集兼容），nlohmann-json 解析 instance.config。

### 3.3 config_schema 校验（自研最小子集）

校验器实现于 `src/plugin/schema_validator.cpp`，支持文档列的最小集合：

| 关键字 | 支持 |
|---|---|
| `type` | object/string/integer/number/boolean/array |
| `required` / `properties` / `default` | ✓ |
| `minimum` / `maximum` / `enum` / `items` | ✓ |
| `secret` | ✓（标记，非校验；用于脱敏） |

失败→`plugin.config.invalid`，错误带具体字段路径。`secret:true` 字段在 log/introspection 中以 `***` 脱敏。

### 3.4 错误模型

`shield_error_v1` 全 phase 统一。code 命名空间：

| 场景 | code |
|---|---|
| manifest 无效 | `plugin.manifest.invalid` |
| package 缺失 | `plugin.package.not_found` |
| 配置无效 | `plugin.config.invalid` |
| 依赖缺失 | `plugin.dependency.missing` |
| 循环依赖 | `plugin.dependency.cycle` |
| ABI 不匹配 | `plugin.abi.mismatch` |
| 入口符号缺失 | `plugin.entry.missing` |
| create 失败 | `plugin.create.failed` |
| init 失败 | `plugin.init.failed` |

`PluginHost::startup` 收集首个 required 失败的完整 error（code/message/hint/package_id/instance_id/phase），结构化记录到 log。

---

## 4. 迁移与集成

### 4.1 DatabasePool 改造（`src/data/data.cpp`）

**旧**（`data.cpp:418-489`）：按 `database.driver` 拼 `shield_db_<driver>` 文件名，自 dlopen + `shield_db_plugin_api()`。

**新**：
```cpp
bool DatabasePool::initialize(std::string_view config_key) {
    // 1. 尝试从 PluginHost 拿 database binding
    auto* db = shield::plugin::global_host()
                   .get_by_binding<shield_database_v1>("database.default");
    // 2. 拿不到 / unavailable → 走 mock（保留现有 mock 降级行为）
    if (!db) { /* mock 分支，不变 */ }
    impl_->plugin = db;   // shield_database_v1* 与 shield_db_plugin* 签名一致
    // 3. 连接参数从 binding instance 的 config 读（host/port/...）
    // 4. 连接池逻辑（pool_size/acquire/timeout）保留不变
}
```

- `PluginDatabaseConnection`（`data.cpp:165`）仅把 `const shield_db_plugin*` → `const shield_database_v1*`，函数指针调用全不变。
- `database.driver` / `database.plugin_path` 配置项废弃；driver 选择改为「binding 指向哪个 package」。
- mock 降级保留（binding 不存在时），CI 不依赖外部服务。

### 4.2 八插件迁移映射

| 现有插件 | 新 package id | interface | 入口变化 |
|---|---|---|---|
| `plugins/sqlite` | `database.sqlite` | `shield.database.v1` | `shield_db_plugin_api`+`shield_plugin_api` → `shield_plugin_get_v1` |
| `plugins/mysql` | `database.mysql` | `shield.database.v1` | 同上 |
| `plugins/postgresql` | `database.postgresql` | `shield.database.v1` | 同上 |
| `plugins/redis`（cache 部分） | `cache.redis` | `shield.cache.v1` | 废除 find_plugin，内嵌 redis 连接 |
| `plugins/redis`（queue 部分） | `queue.redis` | `shield.queue.v1` | 同上 |
| `plugins/redis`（leaderboard 部分） | `leaderboard.redis` | `shield.leaderboard.v1` | 同上 |
| `plugins/auth_jwt` | `auth.jwt` | `shield.auth.v1` | 入口迁移 |
| `plugins/metric_prometheus` | `metrics.prometheus` | `shield.metrics.v1` | 入口迁移 |
| `plugins/health_http` | `health.http` | `shield.health.v1` | 入口迁移 |
| `plugins/matchmaking_elo` |（暂归 `shield.matchmaking.v1`，非首批 7 接口）| 入口迁移 |

**sqlite 迁移样例**（纵切首目标）：现有 `shield_db_plugin_api()`（`shield_db_sqlite.cpp:178`）的静态 `shield_db_plugin` lambda 表 → 改为静态 `shield_database_v1`；新增 `shield_plugin_abi_v1` + `create()` 构造 instance，instance.get_interface 返回该静态 vtable，start/shutdown 管理 sqlite 生命周期。

### 4.3 redis 三件套重构

废除 `plugins/redis/shield_redis.cpp`（公共 redis 基础设施插件，违反文档「不暴露公共 redis 类型」）。

- `redis_plugin.h` 的 `shield_redis_plugin`（connect/get/set/hget...）拆为**内部连接 helper**（`src/plugin/redis_client.h`，非插件、不导出），三件套各自链接。
- 三件套的 `config` 直接声明 redis 连接参数（host/port/password/...），在 `start()` 内各自建连。废除 `find_plugin("shield_redis")` 调用。
- redis 连接实现（hiredis）现状需确认；若现有 shield_redis.cpp 已用 hiredis，则把连接逻辑搬到 helper 复用。

### 4.4 Lua Introspection（`src/lua/lua_api.cpp:2185`）

替换 4 个 stub，对齐文档：

```lua
shield.plugin.packages()             -- [{id,version,kind,provides}]
shield.plugin.instances()            -- [{id,package,state,required}]
shield.plugin.instance("db.main")    -- 单实例状态
shield.plugin.binding("database.default")  -- {instance_id, interface}
```

实现委托 `PluginHost::global_host()` 的 introspection 方法。State 用文档枚举字符串（planned/loaded/started/unavailable/failed/stopped）。

### 4.5 构建系统（CMake）

- **目录布局**：每插件 `plugins/<id>/{manifest.yaml, CMakeLists.txt, *.cpp}`，产出 `bin/libshield_<id>.so`。
- **manifest.yaml 安装**：CMake `configure_file`/`file(COPY)` 将 `manifest.yaml` 与 `.so` 一同部署到 `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/<id>/`。
- **option 保留**：`SHIELD_BUILD_DB_PLUGIN_SQLITE` 等沿用，仅改 target 名与产物布局。
- **新源文件入 build**：`src/plugin/*` 加入 `shield_core`（或新建 `shield_plugin` 静态库，`shield_data` 依赖它以拿 `shield_database_v1` 头与 `global_host`）。
- 删除 `src/core/plugin_manager.cpp` 与 `include/shield/core/plugin_manager.hpp`。

### 4.6 bootstrap 集成

`bootstrap.cpp` 在 `data_init`（:224）之前插入 `plugin_init`：
1. 读 `plugins.*` 子树构造 `PluginConfig`。
2. `shield::plugin::global_host().startup(cfg, err)`。
3. 失败→bootstrap 失败。
4. shutdown 阶段（:233 之后）调 `global_host().shutdown()`。

---

## 5. 实施编排（方案 A）与测试策略

### 5.1 里程碑（纵切优先）

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M0** | 删除旧 PluginManager / 旧 `plugin.h` / 旧入口；新建 ABI 头文件族（abi.h/host_api.h/database.h） | 头文件独立编译通过 |
| **M1** | `src/plugin/` 骨架：Manifest 解析、PluginLibrary、schema_validator、PluginConfig 解析 | 单测：manifest 解析、schema 校验各关键字 |
| **M2** | PluginHost：scan→catalog→plan→resolve→load→create→start 全 pipeline + 拓扑排序 + 错误模型 | 单测：各 phase 失败路径、循环依赖检测 |
| **M3**（纵切端到端） | 迁移 sqlite 插件到新 ABI；DatabasePool 改走 binding；bootstrap 集成 | 集成测试：sqlite 内存库建表/查询/事务跑通；DatabasePool 经 binding 成功 |
| **M4** | Lua introspection（packages/instances/instance/binding） | 单测：Lua 调用返回正确 catalog/runtime 状态 |
| **M5**（横向铺开） | 迁移 mysql/postgresql/auth_jwt/metric_prometheus/health_http/matchmaking_elo | 各插件 smoke test 通过 |
| **M6** | redis 三件套重构（废除 shield_redis，内嵌连接）+ cache/queue/leaderboard interface | 三件套 smoke test；依赖注入验证 |

M3 是关键里程碑：它端到端验证 ABI 设计正确性，是横向铺开的前提。

### 5.2 测试策略

- **单测**（`tests/plugin/`）：manifest 解析、schema 校验、pipeline 各 phase、拓扑排序、错误 code。用现有 CTest + 手写可执行模式。
- **集成测试**：M3 的 sqlite 端到端（编一个最小 host 或直接走 bootstrap + DatabasePool），内存库 `:memory:`，CI 无外部依赖。
- **回归**：现有 `shield_runtime_data_smoke`（CMakeLists:600）等 ctest 必须继续通过。
- **TDD**：M1/M2 每个 phase 先写失败测试再实现（superpowers TDD）。

### 5.3 删除清单

| 文件 | 处置 |
|---|---|
| `include/shield/plugin/plugin.h` | 删除（被 abi.h/host_api.h 取代） |
| `include/shield/plugin/db_plugin.h` | 删除（被 database.h 取代，公共结构搬移） |
| `include/shield/core/plugin_manager.hpp` | 删除 |
| `src/core/plugin_manager.cpp` | 删除 |
| `plugins/redis/shield_redis.cpp` | 删除（redis 三件套重构时） |

---

## 6. Open Decisions（实现中确认）

| 问题 | 倾向 | 确认时机 |
|---|---|---|
| redis 客户端库 | 确认用 **redis-plus-plus**（`sw::redis::*`），hiredis 作为其传递依赖**不显式引入**（`vcpkg.json`/`CMakeLists` 已精简为单库）。redis 三件套内嵌连接复用 `sw::redis::Redis`。 | 已确认 |
| matchmaking 接口归属 | 非首批 7 接口，本次仅迁移入口、暂用 `shield.matchmaking.v1` 占位 | M5 |
| Windows/Linux 库命名是否显式写全 | manifest `library` 三平台都写（对齐文档示例） | M0 |
| 插件 .so 部署目录探测 | 优先 `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/`，回退 host exe 同级 | M3 |
