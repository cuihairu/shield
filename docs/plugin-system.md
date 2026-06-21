# Shield Plugin System v1

本文是 Shield 插件系统的权威设计。`v1` 不兼容旧实验实现。插件系统采用 metadata-first 发现模型，并统一使用 `JSON` 作为 manifest、主配置和插件实例配置格式。

## Goals

| 目标 | 说明 |
| --- | --- |
| Metadata-first | 先扫描 manifest 建立 catalog，不加载插件代码即可知道有哪些包可用。 |
| Discovery / load / start 分离 | 发现、解析依赖、加载共享库、创建实例、启动实例是不同阶段。 |
| JSON only | manifest、主配置、实例配置统一使用 JSON，避免 YAML/TOML/INI 并存。 |
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
| 多格式配置 | 不引入 YAML/TOML/INI。插件和主程序都只讲 JSON。 |

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
  cache.redis/
    plugin.json
    bin/
      shield_cache_redis.dll
      libshield_cache_redis.so
```

主配置指定插件根目录：

```json
{
  "plugins": {
    "directory": "./plugins"
  }
}
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
| `config_schema` | 插件配置 schema，直接使用 JSON Schema 可实现子集表达。 |

## Runtime Config

运行时插件配置也使用 JSON。用户声明要创建哪些实例，以及逻辑 binding 指向哪个实例。

```json
{
  "plugins": {
    "directory": "./plugins",
    "instances": [
      {
        "id": "db.main",
        "package": "database.mysql",
        "required": true,
        "config": {
          "host": "127.0.0.1",
          "port": 3306,
          "database": "shield",
          "username": "root",
          "password": "secret"
        }
      },
      {
        "id": "cache.main",
        "package": "cache.redis",
        "required": false,
        "config": {
          "uri": "redis://127.0.0.1:6379/0"
        }
      }
    ],
    "bindings": {
      "database.default": "db.main",
      "cache.default": "cache.main"
    }
  }
}
```

规则：

| 规则 | 说明 |
| --- | --- |
| 不再有 `plugins.enabled` | 是否启用由 `plugins.instances` 决定。没有实例就只是可用包。 |
| `required: true` | 该实例加载、配置、依赖解析或启动失败时进程启动失败。 |
| `required: false` | 失败时记录结构化错误，实例状态为 `unavailable`，依赖它的 required 实例仍会失败。 |
| `bindings` | 只引用 instance id。host 校验目标实例是否提供对应接口。 |
| 多实例 | 同一个 package 可以创建多个 instance，例如 `db.main` 和 `db.audit`。 |

## Bootstrap Pipeline

启动流程固定为：

```text
scan -> catalog -> plan -> resolve -> load -> create -> start
                                            stop <- shutdown
```

| 阶段 | 说明 |
| --- | --- |
| `scan` | 遍历 `plugins.directory/*/plugin.json`。只读 JSON，不加载共享库。 |
| `catalog` | 校验 manifest schema、package id、平台库路径和 interface 声明。 |
| `plan` | 从主配置的 `plugins.instances` 建立启动计划。 |
| `resolve` | 校验 package 存在、配置合法、依赖可满足、binding 指向合法。 |
| `load` | `dlopen` / `LoadLibrary` 共享库，解析 `entry`。 |
| `create` | 调用入口函数，校验 ABI guard，创建 instance handle。 |
| `start` | 调用实例 `init`，传入 host API、配置和依赖。 |
| `stop` | 进程退出时按依赖反序调用 `shutdown`。 |

## Binary ABI

Manifest 解决发现问题，二进制入口解决真实加载和 ABI 守门问题。两者都必须存在。

```c
#define SHIELD_PLUGIN_ABI_VERSION 1

typedef struct shield_plugin_abi_v1 {
    uint32_t abi_version;
    uint32_t struct_size;
    const char* package_id;
    const char* package_version;
    int (*create)(const shield_plugin_create_args_v1* args,
                  shield_plugin_instance_v1** out,
                  shield_error_v1* err);
} shield_plugin_abi_v1;

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
} shield_host_api_v1;
```

依赖访问规则：

| 规则 | 说明 |
| --- | --- |
| dependency by name | 插件只能访问自己 manifest `requires` 中声明的依赖名。 |
| interface checked | host 校验依赖实例确实提供请求的 interface。 |
| no global registry | 插件不能枚举或任意获取其他插件。 |

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
| `shield.cache.v1` | cache provider，Redis 只是实现之一。 |
| `shield.queue.v1` | queue/pubsub provider。 |
| `shield.leaderboard.v1` | leaderboard provider。 |
| `shield.auth.v1` | authentication provider。 |
| `shield.metrics.v1` | metrics exporter。 |
| `shield.health.v1` | health contributor。 |

Redis 不作为公共基础设施插件类型暴露。需要 Redis 的能力应落到具体 provider：`cache.redis`、`queue.redis`、`leaderboard.redis`。

## Config Schema

配置 schema 由插件声明，host 负责基本类型、必填字段、默认值、范围和 `secret` 标记处理。插件仍可以在 `create` / `init` 中做业务校验。

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

Lua 只暴露查询和状态，不暴露裸 vtable 或跨插件调用。

```lua
local packages = shield.plugin.packages()
local instances = shield.plugin.instances()
local db = shield.plugin.instance("db.main")
local binding = shield.plugin.binding("database.default")
```

返回信息应来自 catalog 和 runtime state：

| API | 说明 |
| --- | --- |
| `shield.plugin.packages()` | 返回可用 package 列表，包括 id、version、kind、provides。 |
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

## Current Rules

v1 的强约束：

| 规则 | 说明 |
| --- | --- |
| 发现来源 | 只扫描 `plugin.json`。 |
| 运行时启用 | 只看 `plugins.instances` 和 `plugins.bindings`。 |
| 二进制入口 | 统一为 `shield_plugin_get_v1()`。 |
| 类型系统 | 以 interface name 为主，例如 `shield.database.v1`。 |
| 依赖注入 | 通过 manifest `requires` 和实例 `dependencies` 解析。 |
| Redis 能力 | 通过具体 provider 暴露，不提供公共基础设施插件类型。 |
| 文档口径 | 当前文档只描述有效设计，不声明历史完成阶段。 |

## Open Decisions

这些问题不阻塞 v1 文档定稿，但实现前需要在 `open-decisions.md` 记录结论：

| 问题 | 候选方向 |
| --- | --- |
| schema 校验实现 | 自研最小子集，或引入现成 JSON Schema validator。 |
| 插件包版本约束 | v1 可先只支持精确 package id，不做 semver range。 |
| 可选实例失败策略 | 当前设计为 `unavailable`，是否允许 binding 自动 fallback 到候选列表需另定。 |
| Windows/Linux 库命名 | 是否要求 manifest 显式写全，还是允许按约定推导。 |
