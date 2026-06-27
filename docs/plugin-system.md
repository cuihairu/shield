# Shield Plugin System v1

本文是 Shield 插件系统的权威设计。`v1` 不兼容旧实验实现。插件系统采用 metadata-first 发现模型：磁盘包用 manifest 描述，运行时实例和 binding 通过 Shield 主配置的 `plugins` 子树声明。

配置口径：

- 插件包 manifest 只支持 `manifest.yaml`。
- `config_schema` 使用 JSON Schema 子集表达。
- `plugins.instances[].config` 是 JSON-compatible 值模型；在当前主程序中它作为 YAML 子树写入 `app.yaml`，由 `Config` 子系统转换为 JSON 值后交给插件系统。
- manifest 的语义模型保持单一：YAML 是唯一磁盘来源，字段结构与本文档定义的 JSON-compatible manifest 一致（YAML 是 JSON 的超集，所有 manifest 字段都能用 JSON-compatible 值模型表达）。

## Goals

| 目标 | 说明 |
| --- | --- |
| Metadata-first | 先扫描 manifest 建立 catalog，不加载插件代码即可知道有哪些包可用。 |
| Discovery / load / start 分离 | 发现、解析依赖、加载共享库、创建实例、启动实例是不同阶段。 |
| JSON-compatible config model | manifest 字段模型与 schema 保持 JSON-compatible；运行时实例配置使用 Shield Config 子树承载，进入插件系统后按 JSON 值模型处理。 |
| Stable C ABI | 插件共享库只暴露 C ABI 入口，不依赖 Shield 内部 C++ 类型。 |
| Explicit wiring | **核心架构决策**：用户在主配置中显式声明每个 instance、binding 和 dependency；host 不做隐式实例创建、不做能力自动发现、不做 starter 模板编排。详见 [Design Rationale](#design-rationale)。 |
| Provider-oriented | 插件提供明确接口，例如 `shield.database.v1`、`shield.cache.v1`。 |
| Structured errors | 加载、配置、依赖和运行期错误都带 `code/message/hint/package/instance/phase`。 |

## Non-goals

| 非目标 | 说明 |
| --- | --- |
| 热加载 | v1 不承诺运行期替换共享库。基础设施热加载与插件系统本质冲突（连接池/TCP 句柄无法搬家、vtable 指针稳定性、并发调用屏障），业界无成功先例。业务代码热重载走 Lua runtime（`require` 重载），与插件热加载是两个层面的问题。详见 [Design Rationale](#design-rationale)。 |
| 请求级生命周期 | v1 只定义进程级 `init` / `shutdown`。worker/request 生命周期后续再加。 |
| 插件间任意互调 | v1 不提供全局 `find_plugin` 或裸 vtable 强转。插件通过声明依赖拿到 host 注入的接口。 |
| 自动配置 / Starter 编排 | v1 不引入 Spring Boot Starter 式的"声明聚合 + 自动配置"层。starter 模式适合标准化 Web 业务（CRUD/REST），不适合游戏后端的异构部署、多实例与高事故代价场景。详见 [Design Rationale](#design-rationale)。 |
| Auto-discovery | v1 不扫描 plugin 目录自动创建实例。隐式实例违背 Explicit Wiring，破坏可审计性。需要快速起步时用 `examples/` 模板项目 copy。 |
| 沙箱 | v1 不隔离 native 插件的内存、线程或系统调用权限。 |
| 多套插件配置入口 | 不为插件系统引入额外 TOML/INI/专用 YAML 文件。运行时配置只来自 Shield 主配置的 `plugins` 子树。 |

## Design Rationale

本节解释 Shield 插件系统为什么以 Explicit Wiring 为核心架构决策，以及为什么不支持热加载、starter 编排和 auto-discovery。这些不是 v1 的临时取舍，而是基于游戏后端固有特征和业界对比得出的长期决策。

### Explicit Wiring 是核心架构决策

**定义**：用户在 `app.yaml` 的 `plugins` 子树中显式声明：

- 每个 instance 的 `id` / `package` / `required` / `dependencies` / `config`
- 每个 binding 的 `logical name` → `instance id`

host 的职责仅限于解析这些声明、注入依赖、调度生命周期。**host 不做的事**：

- 隐式创建实例（"看到 sqlite 包就自动起一个 `db.default`"）
- 能力自动发现（"扫描目录猜你需要哪些插件"）
- Starter 模板编排（"声明 matchmaking 能力，自动拉起完整 stack"）
- 默认 binding（"instance 提供 `shield.database.v1` 就自动绑到 `database.default`"）

所有"自动行为"都被 Explicit Wiring 拒绝——这不是 v1 的能力限制，是基于游戏后端特征的长期架构选择。

### 为什么适合游戏行业

游戏后端有 7 个固有特征，每个都把配置模型推向 Explicit Wiring：

| 固有特征 | 对配置模型的影响 |
| --- | --- |
| 部署异构性极高 | 不同游戏类型用完全不同栈（MMO=MySQL+Redis、SLG=MongoDB+Redis Cluster、FPS=Redis-only）；starter 默认值经常被覆盖，自动配置无收益 |
| 多实例是常态 | `db.main + db.audit + db.shard0..N + db.log`、`cache.session + cache.rank + cache.pubsub`；starter 默认实例根本不够用，最终还是 escape hatch 写 explicit |
| 生产事故代价极高 | 玩家在线=流失+口碑+收入；事故定位每分钟都在烧钱；必须"一眼看配置就知道拓扑"，不允许 starter → override → 展开三层反查 |
| 运维团队规模不大 | 不像互联网大厂有专职 SRE；主程/技术总监常兼任运维；配置可读性 > 简洁性 |
| 复杂依赖需精确控制 | leaderboard 依赖 cache + database；matchmaking 依赖 leaderboard + database + cache；auth 依赖 cache（黑名单）；必须精确指定"哪个实例连哪个实例" |
| 环境差异大 | 开发用 sqlite 内存库，生产用 MySQL cluster；业务代码不变，差异全在配置层 |
| 运营活动 + 紧急 hotfix 频繁 | 配置变更要快速、可逆、可审计；隐式行为阻断紧急回滚 |

Explicit Wiring 在这 7 项里**全部占优**。Starter / Auto-discovery 只在"新项目快速原型"这一边缘场景有收益，且该收益可通过 `examples/` 模板项目满足。

### 业界对比

| 系统 | 类型 | 配置模型 |
| --- | --- | --- |
| Skynet | 游戏服框架（C+Lua） | Explicit |
| Kbengine | 游戏服引擎（C++） | Explicit |
| Photon Server | 游戏服（C#） | Explicit |
| Colyseus | 游戏服（Node） | Explicit |
| Kubernetes | 基础设施 | Explicit（Helm chart 是配置期模板，运行时仍 explicit） |
| Terraform | 基础设施 | Explicit（module 是配置期模板，运行时仍 explicit） |
| Spring Boot | 业务 Web | Starter 主导 |

**关键观察**：所有游戏服框架都用 Explicit Wiring。基础设施系统（K8s/Terraform）即使提供模板层（Helm/Module），运行时模型仍然是 Explicit。Starter 主导只出现在标准化程度高的业务 Web 场景。

Shield 定位是通用游戏后端框架，配置模型与游戏服框架对齐是必然选择。

### 为什么不采用 Starter / Auto-discovery

Starter 模式（Spring Boot 风格的"声明聚合 + 自动配置"）在游戏后端场景下的核心问题：

| 问题 | 表现 |
| --- | --- |
| 多实例场景失效 | 中等规模游戏有 15+ 实例（多分片 db、多用途 cache、多通道 queue），starter 默认的"db + cache + mm"三件套完全不够 |
| 事故定位链变长 | Explicit 一行 grep 3 秒定位；Starter 要查 starter.yaml + override + 展开结果三份，3 秒变 3 分钟 |
| 部署异构性击穿默认值 | 不同游戏 auth 方案（Steam/微信/Firebase/JWT）完全不同，starter 默认值必然被覆盖 |
| 团队维护成本不支撑 | starter 库需要版本管理、profile 测试、兼容性矩阵、文档；中小游戏团队没这个人力 |
| 隐式行为阻断紧急回滚 | starter 升级会隐式改变所有引用方的实际配置，生产环境不可接受 |

Auto-discovery（扫描目录自动创建实例）的问题更严重：隐式实例违背 Explicit Wiring 核心原则，"哪些实例在跑"变成不可预测的状态，调试时无法从配置反推运行态，还有安全风险（恶意放置的 plugin 目录被自动加载）。

需要快速起步时，正确做法是提供 `examples/` 模板项目（含完整 explicit yaml），用户 copy 后修改，而不是引入配置自动展开层。

### 为什么不支持基础设施热加载

"插件热加载"经常与"业务热加载"混淆，但两者是完全不同的问题：

| 维度 | 业务代码热加载 | 基础设施热加载 |
| --- | --- | --- |
| 加载对象 | Lua 脚本 | C ABI 共享库 |
| 变更频率 | 日频甚至小时频 | 季度甚至年级别 |
| 状态依赖 | 无状态或可序列化 | 重状态（连接池、TCP 句柄、事务上下文） |
| 业界先例 | Erlang BEAM / Lua require / JVM JRebel | 无主流系统支持 |
| Shield 支持方式 | 已支持（Lua `require` 重载） | 不支持（架构层面排除） |

基础设施热加载的真实障碍：

- **连接池无法搬家**：TCP 连接是进程级资源，不能在 .so 之间迁移
- **vtable 指针稳定性**：worker 线程刚解引用 vtable，旧 .so 被 dlclose 触发 use-after-free
- **ABI 兼容**：新 .so 的 struct 布局变了，旧 instance 的内存布局失效
- **并发屏障**：vtable 切换瞬间所有 worker 必须进 barrier，实现复杂度爆炸

业界所有"热加载成功"的系统热的都是业务层（字节码/脚本），不是基础设施层。Nginx / PostgreSQL / Redis 升级扩展都走 restart，不走热加载。

**结论**：基础设施热加载的成本远超收益。Shield 业务热重载走 Lua runtime（已有路径），插件层不支持热加载。

### Spring Boot 模式的适用边界

文档对比时常引用 Spring Boot Starter 作为参考。需要明确边界：

| 场景 | Spring Boot Starter 适用 | Shield Explicit Wiring 适用 |
| --- | --- | --- |
| 标准 CRUD 业务 | ✅ | — |
| REST API 服务 | ✅ | — |
| 内部管理后端 | ✅ | — |
| 游戏后端 | — | ✅ |
| 高异构性部署 | — | ✅ |
| 多实例精细控制 | — | ✅ |
| 高事故代价生产环境 | — | ✅ |

Spring Boot Starter 的"约定优于配置"哲学建立在"大部分项目都用同一套"的前提上。游戏后端不满足这个前提——每个游戏的栈都不一样。

借鉴 Spring Boot 可以借鉴的点：接口/实现分离、依赖注入、配置绑定（这些 Shield 都已有）。不应借鉴的点：starter 自动配置、auto-discovery、隐式默认值。

## Terminology

| 名词 | 含义 |
| --- | --- |
| Package | 磁盘上的一个插件包目录，包含 manifest 文件和共享库。 |
| Manifest | `manifest.yaml`，描述包元数据、库路径、接口、依赖和配置 schema。 |
| Catalog | 扫描 manifest 后得到的可用包索引，不要求加载共享库。 |
| Instance | 主配置里创建的一个插件实例。同一个 package 可以有多个 instance。 |
| Binding | 主配置里把一个接口名或逻辑名字绑定到某个 instance，例如 `database.default -> db.main`。 |
| Interface | host 与插件之间的稳定 ABI 契约，例如 `shield.database.v1`。 |
| Capability | 接口下的能力标签，例如 `sql`、`transactions`、`pubsub`。用于查询和选择，不替代接口契约。 |

## Disk Layout

插件目录只扫描 manifest 文件，不扫描所有 DLL/SO。这样可以在不执行第三方代码的情况下建立 catalog。

```text
plugins/
  database.mysql/
    manifest.yaml
    bin/
      shield_database_mysql.dll
      libshield_database_mysql.so
    lua/                      # 可选：插件自带的 Lua 搜索路径
      shield_mysql.lua
  database.mongodb/
    manifest.yaml
    bin/
      shield_doc_mongodb.dll
      libshield_doc_mongodb.so
    lua/
      shield_mongodb.lua
  cache.redis/
    manifest.yaml
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

每个插件包必须有一个 manifest，文件名固定为 `manifest.yaml`。

```yaml
schema_version: 1
id: database.mysql
name: MySQL Database
version: 1.0.0
kind: database
description: MySQL provider for shield.database.v1
entry: shield_plugin_get_v1
library:
  windows: bin/shield_database_mysql.dll
  linux: bin/libshield_database_mysql.so
  macos: bin/libshield_database_mysql.dylib
provides:
  - interface: shield.database.v1
    capabilities: [sql, transactions]
requires: []
lua:
  namespace: database.mysql
  search_paths: [lua/?.lua]
documentation:
  url: https://cuihairu.github.io/shield/plugins/database-mysql
  description: MySQL provider for the shield.database.v1 interface via the X DevAPI
config_schema:
  type: object
  required: [host, database, username]
  properties:
    host:
      type: string
      default: 127.0.0.1
    port:
      type: integer
      default: 3306
      minimum: 1
      maximum: 65535
    database:
      type: string
    username:
      type: string
    password:
      type: string
      secret: true
```

Manifest 字段规则：

| 字段 | 规则 |
| --- | --- |
| `schema_version` | 必须为 `1`。 |
| `id` | 全局唯一 package id，建议使用 `<domain>.<provider>`，如 `database.mysql`。 |
| `version` | package 版本，使用 semver 字符串。 |
| `entry` | 共享库导出的 C 符号，v1 固定建议为 `shield_plugin_get_v1`。 |
| `library` | 按平台声明相对 package 根目录的库路径；必须是相对路径，不能用 `..` 逃逸 package root。 |
| `provides` | 声明 package 提供的 interface 和 capability；至少声明一个 interface，同一 package 内不能重复。 |
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

启动流程固定为 9 个语义阶段：

```text
scan -> catalog -> plan -> resolve -> load -> create -> start -> lua_init -> lua_register
                                                              stop <- shutdown
```

| 阶段 | 说明 |
| --- | --- |
| `scan` | 遍历 `plugins.directory/*/manifest.yaml`。只读 manifest，不加载共享库。 |
| `catalog` | 校验 manifest schema、package id 唯一性、平台库路径安全性和 interface/dependency 声明。 |
| `plan` | 从主配置的 `plugins.instances` 建立启动计划。 |
| `resolve` | 校验 package 存在、实例 id 唯一、依赖可满足、拓扑无环，并在这一阶段应用 config 默认值、执行 schema 校验、校验 binding 名唯一且目标 instance 存在。 |
| `load` | `dlopen` / `LoadLibrary` 共享库，解析 `entry`。 |
| `create` | 调用入口函数，校验 ABI guard，创建 instance handle。 |
| `start` | 调用实例 `start`，插件初始化后端连接、后台线程等资源。 |
| `lua_init` | host 初始化 Lua runtime（包括把所有声明了 `lua.search_paths` 的插件路径注入 `package.path`）。 |
| `lua_register` | host 拿到一个 Lua state 后，遍历所有已 `started` 的实例，依次调用 `register_lua(self, L, err)`，插件把自身 API 注册到该 VM 的 `shield.<namespace>`。多 VM 场景下每个 VM 都会执行一次。 |
| `stop` | 进程退出时按依赖反序调用 `shutdown`。 |

> 实现说明：上表是语义阶段，与代码方法的对应关系是 `scan / catalog / plan_and_resolve（plan + resolve 合并）/ load_all / create_all / start_all`；`lua_init` 和 `lua_register` 是独立方法而非 `start_all` 的延续，仅在 host 配置了 Lua runtime 时调用，纯 C++ 运行模式下跳过。

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

    // host 在每个 Lua VM 初始化后调用一次，插件用 sol2 把自身 API 注册到
    // shield.<namespace>。没有 Lua 绑定的插件也必须提供此字段
    // （直接 return 0 即可）。纯 C++ 运行模式会跳过此回调；被调用时 L 非 NULL。
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

    // 返回字符串值；object/array 返回 JSON fragment。返回指针有效期到同一
    // 线程下一次 config_get 调用。
    const char* (*config_get)(shield_plugin_context_v1* ctx,
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

插件 package 在 `manifest.yaml` 中声明需要哪些依赖：

```yaml
requires:
  - name: cache
    interface: shield.cache.v1
    optional: true
  - name: database
    interface: shield.database.v1
    optional: false
```

主配置在 instance 上绑定依赖（`app.yaml` 的 `plugins` 子树）：

```yaml
plugins:
  instances:
    - id: leaderboard.main
      package: leaderboard.redis
      dependencies:
        cache: cache.main
        database: db.main
      config:
        key_prefix: "lb:"
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
2. **插件自治 API**：由每个插件的 `register_lua` 钩子注册到 `shield.<namespace>`，提供该插件的业务能力（如 `shield.database.mongodb("document.default"):find(...)`）。详见下一节 "Plugin Lua Bindings"。

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
| 多实例 proxy | 同一 package 多个 instance 时，namespace 是 callable table：`shield.database.mongodb("document.default")` 通过 binding 逻辑名返回绑定到目标 instance 的 proxy。 |

### ABI 接入点

`shield_plugin_instance_v1.register_lua(self, L, err)` 是唯一接入点：

- host 在 `lua_register` 阶段遍历所有已 `started` 的实例，按依赖拓扑启动顺序调用。
- `L` 是 host 的 Lua state，保证非 NULL（除非 host 跳过 Lua runtime，此时整个阶段跳过，`register_lua` 不被调用）。
- 插件用 sol2（`sol::state_view(L)`）创建 namespace、注册方法。
- 返回 0 成功；非 0 视为 `register_lua` 失败，host 上报结构化错误。

### namespace 与多实例

namespace 直接来自 manifest 的 `lua.namespace` 字段（或省略时从 `id` 派生）。注册时插件创建一个 callable table：

```lua
-- 业务 Lua 代码
local mongo_default = shield.database.mongodb()                  -- 插件定义的默认 binding
local mongo_audit   = shield.database.mongodb("document.audit")  -- 指定 binding 逻辑名

-- 返回的 proxy 绑定到 binding 解析出的 instance，调用时直接路由到 C ABI
mongo_audit:insert_one("events", { type = "login", user = "alice" })

local cursor = mongo_default:find("users", { age = { ["$gt"] = 18 } })
```

C 侧实现：`register_lua` 创建 `shield.database.mongodb` 这个 table，metatable 的 `__call` 接收 binding 逻辑名，查 PluginHost 解析到对应实例并取得 `shield_document_v1*` 和插件连接句柄，构造 proxy。

### 为什么 Lua 访问用 binding 而非 instance_id

业务代码访问插件时**必须用 binding 逻辑名，不得用 instance_id**（诊断 API 除外）。这是"间接引用"设计——业务依赖角色/语义，不依赖部署实例。理由：

- **部署与代码解耦**：`database.default` 指向哪个 instance 由 `plugins.bindings` 决定，运维迁移、重命名、拆分实例不改业务代码。instance_id 是部署细节，binding 是业务契约。
- **多环境一致**：dev / staging / prod 的 instance_id 必然不同，业务代码恒用 `database.default`。
- **游戏服务器刚需场景**：读写分离（`database.read` → replica）、分片（`database.shard1`）、灰度（`database.canary`）、故障切换——用 binding 都是改配置，用 instance_id 都要改业务代码。
- **测试**：binding 指向 mock instance，业务代码零改动。
- **与 C++ 路径统一**：C++ 消费者经 `PluginHost::get_by_binding<T>(binding)` 访问，Lua 走同一套 `plugins.bindings`，两侧语义一致，避免"C++ 用 binding、Lua 用 instance_id"的割裂。
- **业界先例**：DNS（域名→IP）、Kubernetes Service（service name→pod）、Spring `@Qualifier`、Envoy cluster——长期维护的资源访问都采用逻辑引用而非物理标识。直接用 instance_id 等于让业务代码写 IP 而非域名。

#### 使用规则

1. 业务 Lua 只传 binding：`shield.database.mysql("database.default")` ✓；`shield.database.mysql("db.main")` ✗（`db.main` 是 instance_id）。
2. binding 命名约定 `<interface-type>.<purpose>`：`database.default` / `database.read` / `cache.session` / `queue.events` / `leaderboard.arena`。
3. binding 在主配置 `plugins.bindings` 声明（`logical: instance_id`），**运维拥有**，不在业务代码硬编码。
4. instance_id 只出现在配置 `plugins.instances[].id` 与只读诊断 API（`shield.plugin.instances()`）；业务代码不直接用。
5. 缺失/未配置 binding → `nil, { code = "module_unavailable" }`（软失败，不 panic）。
6. 诊断/调试可直接指定 instance_id，但只能经专门诊断入口，非常规业务路径。

> **实现要求**：插件 `register_lua` 创建的 callable table，其 `__call` **必须接收 binding 逻辑名并经 PluginHost 解析到 instance，不得直接用 instance_id 查插件内部表**。当前 mysql / cache.redis 等数据插件的 `__call` 仍按 instance_id 查内部 registry（`find_instance(instance_id)`），属于待修正的实现偏差，与本文设计不符。

### Lua 文件位置

```
plugins/mongodb/
├── shield_doc_mongodb.cpp        # C ABI 实现
├── lua_bindings.cpp              # register_lua：创建 namespace + 方法
├── lua/                          # 可选：纯 Lua 业务封装
│   ├── init.lua
│   └── shield_mongodb.lua
├── manifest.yaml
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
| 发现来源 | 仅扫描 `manifest.yaml`。 |
| 运行时启用 | 只看 `plugins.instances` 和 `plugins.bindings`。 |
| 二进制入口 | 统一为 `shield_plugin_get_v1()`。 |
| 类型系统 | 以 interface name 为主，例如 `shield.database.v1`、`shield.document.v1`。 |
| 依赖注入 | 通过 manifest `requires` 和实例 `dependencies` 解析。 |
| Redis 能力 | 通过具体 provider 暴露，不提供公共基础设施插件类型。 |
| Lua 自治 | 插件必须实现 `register_lua` 钩子。业务 Lua 绑定跟随插件目录，host 端 `src/lua/lua_api.cpp` 不感知具体插件。 |
| namespace 派生 | Lua namespace 直接来自 manifest `lua.namespace`（或从 `id` 派生）。多实例用 `shield.<ns>(binding)` proxy。 |
| 文档口径 | 当前文档只描述有效设计，不声明历史完成阶段。 |

## Implementation Status

当前实现已经具备插件系统主路径：manifest 扫描、catalog、依赖拓扑、动态库加载、ABI 校验、实例创建、启动、C++ binding 访问、Lua path 注入、`register_lua` 分发和只读 introspection。

旧插件包需要迁移到 `manifest.yaml` 后才能被 host 发现。

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
