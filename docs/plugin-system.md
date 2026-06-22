# Shield Plugin System v1

本文是 Shield 插件系统的权威设计。`v1` 不兼容旧实验实现。插件系统采用 metadata-first 发现模型：磁盘包用 `plugin.json` 描述，运行时实例和 binding 通过 Shield 主配置的 `plugins` 子树声明。

配置口径：

- `plugin.json` 必须是真 JSON 文件。
- `config_schema` 使用 JSON Schema 子集表达。
- `plugins.instances[].config` 是 JSON-compatible 值模型；在当前主程序中它作为 YAML 子树写入 `app.yaml`，由 `Config` 子系统转换为 JSON 值后交给插件系统。
- 插件系统不再引入独立的 TOML/INI/YAML 解析器，也不维护第二套配置入口。

## Goals

| 目标 | 说明 |
| --- | --- |
| Metadata-first | 先扫描 manifest 建立 catalog，不加载插件代码即可知道有哪些包可用。 |
| Discovery / load / start 分离 | 发现、解析依赖、加载共享库、创建实例、启动实例是不同阶段。 |
| JSON-compatible config model | manifest 和 schema 使用 JSON；运行时实例配置使用 Shield Config 子树承载，进入插件系统后按 JSON 值模型处理。 |
| Stable C ABI | 插件共享库只暴露 C ABI 入口，不依赖 Shield 内部 C++ 类型。 |
| Explicit wiring | 用户在主配置中声明实例和绑定关系，host 负责解析依赖和注入。 |
| Provider-oriented | 插件提供明确接口，例如 `shield.database.v1`、`shield.cache.v1`。 |
| Structured errors | 加载、配置、依赖和运行期错误都带 `code/message/hint/package/instance/phase`。 |

## Non-goals

| 非目标 | 说明 |
| --- | --- |
| 热加载 | v1 不承诺运行期替换共享库。 |
| 请求级生命周期 | v1 只定义进程级 `init` / `shutdown`。worker/request 生命周期后续再加。 |
| 插件间任意互调 | v1 不提供全局 `find_plugin` 或裸 vtable 强转。插件通过声明依赖拿到 host 注入的接口。 |
| 沙箱 | v1 不隔离 native 插件的内存、线程或系统调用权限。 |
| 多套插件配置入口 | 不为插件系统引入额外 TOML/INI/专用 YAML 文件。运行时配置只来自 Shield 主配置的 `plugins` 子树。 |

## Terminology

| 名词 | 含义 |
| --- | --- |
| Package | 磁盘上的一个插件包目录，包含 `plugin.json` 和共享库。 |
| Manifest | `plugin.json`，描述包元数据、库路径、接口、依赖和配置 schema。 |
| Catalog | 扫描 manifest 后得到的可用包索引，不要求加载共享库。 |
| Instance | 主配置里创建的一个插件实例。同一个 package 可以有多个 instance。 |
| Binding | 主配置里把一个接口名或逻辑名字绑定到某个 instance，例如 `database.default -> db.main`。 |
| Interface | host 与插件之间的稳定 ABI 契约，例如 `shield.database.v1`。 |
| Capability | 接口下的能力标签，例如 `sql`、`transactions`、`pubsub`。用于查询和选择，不替代接口契约。 |

## Disk Layout

插件目录只扫描 `plugin.json`，不扫描所有 DLL/SO。这样可以在不执行第三方代码的情况下建立 catalog。

```text
plugins/
  database.mysql/
    plugin.json
    bin/
      shield_database_mysql.dll
      libshield_database_mysql.so
    lua/                      # 可选：插件自带的 Lua 搜索路径
      shield_mysql.lua
  database.mongodb/
    plugin.json
    bin/
      shield_doc_mongodb.dll
      libshield_doc_mongodb.so
    lua/
      shield_mongodb.lua
  cache.redis/
    plugin.json
    bin/
      shield_cache_redis.dll
      libshield_cache_redis.so
```

带 `lua/` 子目录的插件，host 会在 Lua runtime 初始化后把 `lua/?.lua` 注入 `package.path`，并通过插件的 `register_lua` 钩子把 `shield.<package>.*` 注册到 Lua 侧。

主配置指定插件根目录。当前主程序使用 `app.yaml`：

```yaml
plugins:
  directory: "./plugins"
```

## Manifest

每个插件包必须有一个 `plugin.json`：

```json
{
  "schema_version": 1,
  "id": "database.mysql",
  "name": "MySQL Database",
  "version": "1.0.0",
  "kind": "database",
  "description": "MySQL provider for shield.database.v1",
  "entry": "shield_plugin_get_v1",
  "library": {
    "windows": "bin/shield_database_mysql.dll",
    "linux": "bin/libshield_database_mysql.so",
    "macos": "bin/libshield_database_mysql.dylib"
  },
  "provides": [
    {
      "interface": "shield.database.v1",
      "capabilities": ["sql", "transactions"]
    }
  ],
  "requires": [],
  "lua": {
    "namespace": "database.mysql",
    "search_paths": ["lua/?.lua"]
  },
  "documentation": {
    "url": "https://cuihairu.github.io/shield/plugins/database-mysql",
    "description": "MySQL provider for the shield.database.v1 interface via the X DevAPI"
  },
  "config_schema": {
    "type": "object",
    "required": ["host", "database", "username"],
    "properties": {
      "host": {
        "type": "string",
        "default": "127.0.0.1"
      },
      "port": {
        "type": "integer",
        "default": 3306,
        "minimum": 1,
        "maximum": 65535
      },
      "database": {
        "type": "string"
      },
      "username": {
        "type": "string"
      },
      "password": {
        "type": "string",
        "secret": true
      }
    }
  }
}
```

Manifest 字段规则：

| 字段 | 规则 |
| --- | --- |
| `schema_version` | 必须为 `1`。 |
| `id` | 全局唯一 package id，建议使用 `<domain>.<provider>`，如 `database.mysql`。 |
| `version` | package 版本，使用 semver 字符串。 |
| `entry` | 共享库导出的 C 符号，v1 固定建议为 `shield_plugin_get_v1`。 |
| `library` | 按平台声明相对 package 根目录的库路径。 |
| `provides` | 声明 package 提供的 interface 和 capability。 |
| `requires` | 声明 instance 创建时需要 host 注入的依赖接口。 |
| `lua` | 可选。声明插件的 Lua 元数据：`namespace`（Lua 侧注册到 `shield.<namespace>`）、`search_paths`（相对 package 根的 Lua 搜索路径，host 启动时自动注入 `package.path`）。 |
| `documentation` | 可选。声明插件的在线文档 URL 和一句话描述。host 通过 `PluginHost::list_packages()` 暴露这份元数据，当前 `shield.plugin.packages()` Lua API 会直接返回 `docs_url` 和 `docs_description`。第三方插件**强烈推荐**设置此字段。 |
| `config_schema` | 插件配置 schema，直接使用 JSON Schema 可实现子集表达。 |

## Runtime Config

运行时插件配置写在 Shield 主配置的 `plugins` 子树中。当前仓库默认使用 YAML 文件，但该子树必须保持 JSON-compatible：object、array、string、number、boolean、null，不使用 YAML 特有语义作为插件契约。

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: db.main
      package: database.mysql
      required: true
      config:
        host: "127.0.0.1"
        port: 3306
        database: shield
        username: root
        password: secret

    - id: cache.main
      package: cache.redis
      required: false
      config:
        uri: "redis://127.0.0.1:6379/0"

  bindings:
    database.default: db.main
    cache.default: cache.main
```

规则：

| 规则 | 说明 |
| --- | --- |
| 不再有 `plugins.enabled` | 是否启用由 `plugins.instances` 决定。没有实例就只是可用包。 |
| `required: true` | 该实例加载、配置、依赖解析或启动失败时进程启动失败。 |
| `required: false` | 失败时记录结构化错误，实例状态为 `unavailable`，依赖它的 required 实例仍会失败。 |
| `bindings` | 当前格式为 `logical_name: instance_id`。C++ 调用方通过 `get_by_binding<T>()` 提供目标 interface，host 在访问时向实例请求该 interface。 |
| 多实例 | 同一个 package 可以创建多个 instance，例如 `db.main` 和 `db.audit`。 |

> 说明：显式 interface binding（例如 `{ instance: "db.main", interface: "shield.database.v1" }`）仍是可选演进方向；v1 当前配置格式保持最小化，避免在 binding 层重复声明 package manifest 已经提供的信息。

## Bootstrap Pipeline

启动流程固定为：

```text
scan -> catalog -> plan -> resolve -> load -> create -> start -> lua_init -> lua_register
                                                              stop <- shutdown
```

| 阶段 | 说明 |
| --- | --- |
| `scan` | 遍历 `plugins.directory/*/plugin.json`。只读 JSON，不加载共享库。 |
| `catalog` | 校验 manifest schema、package id、平台库路径和 interface 声明。 |
| `plan` | 从主配置的 `plugins.instances` 建立启动计划。 |
| `resolve` | 校验 package 存在、实例 id 唯一、依赖可满足、拓扑无环，并在这一阶段应用 config 默认值、执行 schema 校验、校验 binding 名唯一且目标 instance 存在。 |
| `load` | `dlopen` / `LoadLibrary` 共享库，解析 `entry`。 |
| `create` | 调用入口函数，校验 ABI guard，创建 instance handle。 |
| `start` | 调用实例 `start`，插件初始化后端连接、后台线程等资源。 |
| `lua_init` | host 初始化 Lua runtime（包括把所有声明了 `lua.search_paths` 的插件路径注入 `package.path`）。 |
| `lua_register` | host 拿到 Lua state 后，遍历所有已 `started` 的实例，依次调用 `register_lua(self, L, err)`，插件把自身 API 注册到 `shield.<namespace>`。 |
| `stop` | 进程退出时按依赖反序调用 `shutdown`。 |

`lua_init` 和 `lua_register` 只在 host 配置了 Lua runtime 时触发；纯 C++ 运行模式下这两个阶段跳过，`register_lua` 钩子不会被调用。

## Binary ABI

Manifest 解决发现问题，二进制入口解决真实加载和 ABI 守门问题。两者都必须存在。

```c
#define SHIELD_PLUGIN_ABI_VERSION 1

struct lua_State;  // 前向声明，abi.h 不直接 include lua.h

typedef struct shield_plugin_abi_v1 {
    uint32_t abi_version;
    uint32_t struct_size;
    const char* package_id;
    const char* package_version;
    int (*create)(const shield_plugin_create_args_v1* args,
                  shield_plugin_instance_v1** out,
                  shield_error_v1* err);
} shield_plugin_abi_v1;

typedef struct shield_plugin_instance_v1 {
    uint32_t struct_size;
    const char* instance_id;

    const void* (*get_interface)(shield_plugin_instance_v1* self,
                                 const char* interface_name,
                                 shield_error_v1* err);

    int (*start)(shield_plugin_instance_v1* self,
                 shield_error_v1* err);

    void (*shutdown)(shield_plugin_instance_v1* self);

    // host 在 Lua runtime 起来后调用一次，插件用 sol2 把自身 API 注册到
    // shield.<namespace>。没有 Lua 绑定的插件也必须提供此字段
    // （直接 return 0 即可）。L 可能为 NULL（纯 C++ 运行模式）。
    int (*register_lua)(shield_plugin_instance_v1* self,
                        struct lua_State* L,
                        shield_error_v1* err);
} shield_plugin_instance_v1;

SHIELD_PLUGIN_EXPORT
const shield_plugin_abi_v1* shield_plugin_get_v1(void);
```

ABI 校验规则：

| 校验项 | 失败处理 |
| --- | --- |
| 入口符号不存在 | load 失败。 |
| `abi_version != 1` | load 失败。 |
| `struct_size` 小于 host 需要的最小大小 | load 失败。 |
| `package_id` 与 manifest `id` 不一致 | load 失败。 |
| 共享库不提供 manifest 声明的 interface | create 失败。 |

## Host API

host API 只提供运行环境能力和已解析依赖，不提供全局插件查找。

```c
typedef struct shield_host_api_v1 {
    void (*log)(shield_log_level level,
                const char* plugin,
                const char* instance,
                const char* message);

    void (*report_error)(const shield_error_v1* err);

    const shield_config_value_v1* (*config_get)(shield_plugin_context_v1* ctx,
                                                const char* path);

    const void* (*dependency)(shield_plugin_context_v1* ctx,
                              const char* name,
                              const char* interface_name);

    // 返回 host 的 lua_State*。如果 host 以纯 C++ 模式运行（未启 Lua
    // runtime），返回 NULL。插件 register_lua 被调用时此值保证非 NULL。
    struct lua_State* (*lua_state)(shield_plugin_context_v1* ctx);

    // 把一个路径加入 Lua package.path（is_cpath=0）或 package.cpath
    // （is_cpath=1）。相对路径以插件 package 根目录为基准解析为绝对路径。
    // 多次调用累加。返回 0 成功，非 0 失败。
    int (*lua_add_path)(shield_plugin_context_v1* ctx,
                        const char* path,
                        int is_cpath);
} shield_host_api_v1;
```

依赖访问规则：

| 规则 | 说明 |
| --- | --- |
| dependency by name | 插件只能访问自己 manifest `requires` 中声明的依赖名。 |
| interface checked | host 校验依赖实例确实提供请求的 interface。 |
| no global registry | 插件不能枚举或任意获取其他插件。 |
| lua access | 插件通过 `lua_state` 拿到 host 的 Lua state；通过 `lua_add_path` 动态扩展 Lua 搜索路径（manifest `lua.search_paths` 是声明式快捷方式）。 |

## Dependency Model

插件 package 在 manifest 中声明需要哪些依赖：

```json
{
  "requires": [
    {
      "name": "cache",
      "interface": "shield.cache.v1",
      "optional": true
    },
    {
      "name": "database",
      "interface": "shield.database.v1",
      "optional": false
    }
  ]
}
```

主配置在 instance 上绑定依赖：

```json
{
  "plugins": {
    "instances": [
      {
        "id": "leaderboard.main",
        "package": "leaderboard.redis",
        "dependencies": {
          "cache": "cache.main",
          "database": "db.main"
        },
        "config": {
          "key_prefix": "lb:"
        }
      }
    ]
  }
}
```

规则：

| 规则 | 说明 |
| --- | --- |
| 必需依赖缺失 | resolve 失败。 |
| 可选依赖缺失 | create/start 时传入 `null`，插件必须降级或报错。 |
| 循环依赖 | resolve 失败。 |
| 启动顺序 | 依赖先 start，被依赖者后 stop。 |

## Interface Model

接口名是稳定 ABI 契约，不使用单一 `shield_plugin_type` 枚举承载所有类型。新增接口不要求修改全局枚举。

第一批建议接口：

| Interface | 用途 |
| --- | --- |
| `shield.database.v1` | SQL database provider。 |
| `shield.document.v1` | 文档数据库 provider（MongoDB 等），collection 级 CRUD + 聚合管道 + 事务。 |
| `shield.cache.v1` | cache provider，Redis 只是实现之一。 |
| `shield.queue.v1` | queue/pubsub provider。 |
| `shield.leaderboard.v1` | leaderboard provider。 |
| `shield.auth.v1` | authentication provider。 |
| `shield.metrics.v1` | metrics exporter。 |
| `shield.health.v1` | health contributor。 |

`shield.document.v1` 与 `shield.database.v1` 并列而非继承：文档库的 filter / aggregation / _id 语义与 SQL 差距过大，强行套用 SQL 接口（query/execute/last_insert_id）会丢掉文档库的核心价值。两个接口可以由不同 package 各自提供。

Redis 不作为公共基础设施插件类型暴露。需要 Redis 的能力应落到具体 provider：`cache.redis`、`queue.redis`、`leaderboard.redis`。

## Config Schema

配置 schema 由插件声明。v1 目标语义是 host 负责基本类型、必填字段、默认值、范围和 `secret` 标记处理；插件仍可以在 `create` / `start` 中做业务校验。

v1 schema 支持以下最小集合：

| 关键字 | 说明 |
| --- | --- |
| `type` | `object`、`string`、`integer`、`number`、`boolean`、`array`。 |
| `required` | object 必填字段列表。 |
| `properties` | object 字段定义。 |
| `default` | 默认值。 |
| `minimum` / `maximum` | 数值范围。 |
| `enum` | 字符串或数字枚举。 |
| `items` | array 元素 schema。 |
| `secret` | 日志和 introspection 中脱敏。 |

## Error Model

错误对象结构：

```c
typedef struct shield_error_v1 {
    const char* code;
    const char* message;
    const char* hint;
    const char* package_id;
    const char* instance_id;
    const char* phase;
} shield_error_v1;
```

错误语义：

| 场景 | code 示例 |
| --- | --- |
| manifest 无效 | `plugin.manifest.invalid` |
| package 缺失 | `plugin.package.not_found` |
| 配置无效 | `plugin.config.invalid` |
| 依赖缺失 | `plugin.dependency.missing` |
| ABI 不匹配 | `plugin.abi.mismatch` |
| 入口符号缺失 | `plugin.entry.missing` |
| init 失败 | `plugin.init.failed` |

多 provider 路由允许软拒绝语义：如果某接口方法是 selector/dispatcher 类型，插件可以返回 `SHIELD_DECLINED` 表示“不处理，让下一个候选处理”。真正错误返回 `SHIELD_ERROR` 并携带 `shield_error_v1`。

## Lua Introspection

Lua 侧有两类 API：

1. **Host 内置 API**：由 host 的 `src/lua/lua_api.cpp` 注册，提供运行时核心能力（`shield.spawn` / `shield.send` / `shield.timer` / `shield.log` / `shield.config` / `shield.plugin.*` 等）。
2. **插件自治 API**：由每个插件的 `register_lua` 钩子注册到 `shield.<namespace>`，提供该插件的业务能力（如 `shield.database.mongodb("doc.main"):find(...)`）。详见下一节 "Plugin Lua Bindings"。

Host 内置 API 只暴露查询和状态，不暴露裸 vtable 或跨插件调用：

```lua
local packages = shield.plugin.packages()
local instances = shield.plugin.instances()
local db = shield.plugin.instance("db.main")
local binding = shield.plugin.binding("database.default")
```

返回信息应来自 catalog 和 runtime state：

| API | 说明 |
| --- | --- |
| `shield.plugin.packages()` | 返回可用 package 列表，包括 id、version、kind、provides、docs_url、docs_description。 |
| `shield.plugin.instances()` | 返回实例列表，包括 id、package、state、required。 |
| `shield.plugin.instance(id)` | 返回单个实例状态。 |
| `shield.plugin.binding(name)` | 返回 binding 指向的 instance id 和 interface。 |

状态枚举：

| State | 含义 |
| --- | --- |
| `planned` | 已进入启动计划，尚未加载。 |
| `loaded` | 共享库已加载，ABI 已校验。 |
| `started` | `init` 成功。 |
| `unavailable` | 非 required 实例失败并被跳过。 |
| `failed` | required 实例失败，启动流程会失败。 |
| `stopped` | 已调用 `shutdown`。 |

## Plugin Lua Bindings

插件不只是 C ABI 的载体，还要自带 Lua 绑定和业务封装 Lua 文件。这让每个插件目录真正自包含——host 端代码零修改即可引入新插件。

### 设计原则

| 原则 | 说明 |
| --- | --- |
| 插件自治 | C ABI + Lua 绑定 + Lua 业务文件，全在 `plugins/<package>/` 下，不污染 host。 |
| namespace 统一 | 插件 package id 直接映射 Lua namespace：`database.mongodb` → `shield.database.mongodb`。 |
| 显式 register_lua | 每个 instance 必须实现 `register_lua` 钩子，即使空实现。host 不猜测插件是否需要 Lua。 |
| 双层路径注入 | manifest `lua.search_paths` 是声明式快捷方式；`host_api.lua_add_path` 是命令式兜底，用于运行时动态决策。 |
| 多实例 proxy | 同一 package 多个 instance 时，namespace 是 callable table：`shield.database.mongodb("doc.main")` 返回绑定到该实例的 proxy。 |

### ABI 接入点

`shield_plugin_instance_v1.register_lua(self, L, err)` 是唯一接入点：

- host 在 `lua_register` 阶段遍历所有已 `started` 的实例，按 instance 创建顺序调用。
- `L` 是 host 的 Lua state，保证非 NULL（除非 host 跳过 Lua runtime，此时整个阶段跳过，`register_lua` 不被调用）。
- 插件用 sol2（`sol::state_view(L)`）创建 namespace、注册方法。
- 返回 0 成功；非 0 视为 `register_lua` 失败，host 上报结构化错误。

### namespace 与多实例

namespace 直接来自 manifest 的 `lua.namespace` 字段（或省略时从 `id` 派生）。注册时插件创建一个 callable table：

```lua
-- 业务 Lua 代码
local mongo_default = shield.database.mongodb()            -- 默认实例（binding "document.default"）
local mongo_audit   = shield.database.mongodb("doc.audit") -- 指定实例

-- 返回的 proxy 绑定到该 instance，调用时直接路由到 C ABI
mongo_audit:insert_one("events", { type = "login", user = "alice" })

local cursor = mongo_default:find("users", { age = { ["$gt"] = 18 } })
```

C 侧实现：`register_lua` 创建 `shield.database.mongodb` 这个 table，metatable 的 `__call` 接收 instance_id，查 PluginHost 拿到对应实例的 `shield_document_v1*` 和 `shield_doc_conn*`，构造 proxy。

### Lua 文件位置

```
plugins/mongodb/
├── shield_doc_mongodb.cpp        # C ABI 实现
├── lua_bindings.cpp              # register_lua：创建 namespace + 方法
├── lua/                          # 可选：纯 Lua 业务封装
│   ├── init.lua
│   └── shield_mongodb.lua
├── plugin.json
└── CMakeLists.txt
```

带 `lua/` 目录的插件，host 启动时：

1. 扫描 manifest 的 `lua.search_paths` 字段，把 `<plugin_dir>/lua/?.lua` 注入 `package.path`（在 `lua_init` 阶段）。
2. 调用 `register_lua`，插件自行决定是否 `require "shield_mongodb"` 加载业务封装。

### register_lua 示例（插件侧）

```cpp
int my_register_lua(shield_plugin_instance_v1* self,
                    lua_State* L,
                    shield_error_v1* err) {
    sol::state_view lua(L);
    sol::table shield = lua["shield"].get_or_create<sol::table>();
    sol::table mongodb = shield["database"].get_or_create<sol::table>()
                              ["mongodb"].get_or_create<sol::table>();

    // callable table: mongo(id) -> proxy
    mongodb.set_function(sol::call,
        [](std::string instance_id) -> sol::table {
            // 查 host 拿到该实例的 shield_document_v1* + shield_doc_conn*
            // 构造 proxy table，注册 find/insert_one/... 方法
            return make_proxy(instance_id);
        });

    return 0;
}
```

### host 端实现要点

| 实现 | 文件 |
| --- | --- |
| `register_lua_all(L)` | `src/plugin/plugin_host.cpp` — 遍历 started 实例，调用 `register_lua` |
| `lua_state` host_api 回调 | `src/plugin/plugin_host.cpp` — 返回 `LuaRuntime::raw_state()` |
| `lua_add_path` host_api 回调 | `src/plugin/plugin_host.cpp` — 改写 `package.path` |
| Manifest 解析 `lua` 字段 | `src/plugin/manifest.cpp` — 加 `PluginLuaMeta` |
| Bootstrap 时序 | `src/bootstrap/bootstrap.cpp` — `start_all` → `lua_init` → `register_lua_all` |

### 当前 namespace 列表

| Package | Lua namespace | 主要方法 |
| --- | --- | --- |
| `database.sqlite` | `shield.database.sqlite` | query / execute / transaction |
| `database.mysql` | `shield.database.mysql` | query / execute / transaction |
| `database.postgresql` | `shield.database.postgresql` | query / execute / transaction |
| `database.mongodb` | `shield.database.mongodb` | find / insert_one / update_one / delete_one / aggregate / count / transaction |
| `cache.redis` | `shield.cache.redis` | get / set / del / incr / hget / hset |
| `queue.redis` | `shield.queue.redis` | publish / subscribe / unsubscribe |
| `leaderboard.redis` | `shield.leaderboard.redis` | set_entry / get_rank / top_n / remove_entry |
| `auth.jwt` | 暂无 | 当前仅提供 C ABI，Lua 绑定未实现 |
| `metrics.prometheus` | 暂无 | 当前仅提供 C ABI，Lua 绑定未实现 |
| `health.http` | 暂无 | 当前仅提供 C ABI，Lua 绑定未实现 |
| `matchmaking.elo` | 暂无 | 当前仅提供 C ABI，Lua 绑定未实现 |

## Current Rules

v1 的强约束：

| 规则 | 说明 |
| --- | --- |
| 发现来源 | 只扫描 `plugin.json`。 |
| 运行时启用 | 只看 `plugins.instances` 和 `plugins.bindings`。 |
| 二进制入口 | 统一为 `shield_plugin_get_v1()`。 |
| 类型系统 | 以 interface name 为主，例如 `shield.database.v1`、`shield.document.v1`。 |
| 依赖注入 | 通过 manifest `requires` 和实例 `dependencies` 解析。 |
| Redis 能力 | 通过具体 provider 暴露，不提供公共基础设施插件类型。 |
| Lua 自治 | 插件必须实现 `register_lua` 钩子。业务 Lua 绑定跟随插件目录，host 端 `src/lua/lua_api.cpp` 不感知具体插件。 |
| namespace 派生 | Lua namespace 直接来自 manifest `lua.namespace`（或从 `id` 派生）。多实例用 `shield.<ns>(instance_id)` proxy。 |
| 文档口径 | 当前文档只描述有效设计，不声明历史完成阶段。 |

## Implementation Status

当前实现已经具备插件系统主路径：`plugin.json` 扫描、catalog、依赖拓扑、动态库加载、ABI 校验、实例创建、启动、C++ binding 访问、Lua path 注入、`register_lua` 分发和只读 introspection。

以下条目是 v1 收敛项，文档中按目标语义描述，但实现仍需补齐：

| 项目 | 当前状态 | 目标 |
| --- | --- | --- |
| `config_schema` 校验 | resolve 阶段已应用默认值，并执行类型/必填/范围/enum 校验。 | 继续扩展 JSON Schema 子集和 secret 脱敏输出。 |
| `required: false` | optional 实例在 package/load/create/start 失败时进入 `unavailable`；required 实例失败会中断启动。 | 后续可增加 binding fallback 候选列表。 |
| binding 校验 | resolve 阶段校验 binding 名唯一且目标 instance 存在；访问时仍由 `get_by_binding<T>()` 请求 interface。 | 必要时增加显式 interface binding 格式。 |
| 结构化错误 | C ABI 有 `shield_error_v1`；host 侧错误字符串仍有部分路径是拼接文本。 | 所有阶段统一携带 `code/message/hint/package/instance/phase`。 |

## Open Decisions

这些问题不阻塞 v1 文档定稿，但实现前需要在 `open-decisions.md` 记录结论：

| 问题 | 候选方向 |
| --- | --- |
| schema 校验实现 | 当前已有自研最小子集；后续决定是否扩展或替换为现成 JSON Schema validator。 |
| 插件包版本约束 | v1 可先只支持精确 package id，不做 semver range。 |
| 可选实例失败策略 | 当前设计为 `unavailable`，是否允许 binding 自动 fallback 到候选列表需另定。 |
| Windows/Linux 库命名 | 是否要求 manifest 显式写全，还是允许按约定推导。 |
