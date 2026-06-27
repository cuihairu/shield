# 数据访问架构

Shield 的数据访问（SQL、文档、KV、队列、排行榜）采用**纯插件架构**：核心 runtime 不含任何数据库代码，所有 DB/Redis 能力由独立插件提供。本文是数据主题的总览；权威契约见 [Lua API](lua-api.md) 与 [插件系统 v1](plugin-system.md)。

## 历史说明：shield_data 规划已废弃

历史上曾规划过一个 `shield_data` 核心模块（提供 `DatabasePool` / `DatabaseConnection` 和 `shield.db.*` / `shield.redis.*` Lua API），但**该规划从未落地**：

- `include/shield/` 下没有 `data` 子目录，`CMakeLists.txt` 没有 `shield_data` target（它只出现在 [CMake 重构](cmake-refactor.md) 的规划表里）。
- `src/lua/lua_api.cpp` 不注册任何 db/redis 模块。
- 本文早期版本描述的 `shield_data` / `DatabasePool` / `shield.db:*` / `shield.redis:*` / "CTest 已覆盖" 等内容均为虚构，已废弃。

数据访问的真实形态见下文。如与 [Lua API 契约](lua-api.md) 冲突，以 lua-api.md 为准——它已把 `shield.db:*` / `shield.redis:*` 列入"删除的旧 API"。数据访问按插件 namespace 调用，形态是 `shield.<plugin-namespace>(binding)`：`binding` 是配置（`plugins.bindings`）中声明的**逻辑名**，host 内部经它解析到 plugin instance；同一 binding 名在配置与 Lua 调用中须一致。具体方法名以 [Lua API 契约](lua-api.md) 为准。下为伪代码示意（`<method>` 代表各插件的方法，非可执行 Lua）：

```text
shield.database.mysql("database.default"):<method>(...)      -- SQL（mysql/postgresql 同形）
shield.database.mongodb("document.default"):<method>(...)    -- 文档
shield.cache.redis("cache.session"):<method>(...)            -- KV 缓存
shield.queue.redis("queue.events"):<method>(...)             -- 队列
shield.leaderboard.redis("leaderboard.arena"):<method>(...)  -- 排行榜
```

binding 而非 instance_id 的设计理由与命名规则见 [插件系统 · 为什么用 binding](plugin-system.md#为什么-lua-访问用-binding-而非-instance-id)。

## 架构：core 零数据 + 插件自治

```text
Lua 业务 ──shield.database.mysql("database.default"):<method>(...)──┐
                                                         │ 插件自注册的 Lua namespace
                                                         ▼
                              每个 Lua VM 初始化时 PluginHost::register_lua_all
                                         调各插件 register_lua 安装 shield.<ns> 表
                                                         │
                          ┌──────────────────────────────┴──────────────────────┐
                          ▼ (Lua 路径)                       (C++ 路径)            ▼
                  插件 register_lua 建表            PluginHost::get_by_binding<T>
                          │                                       │
                          └─────────────────┬─────────────────────┘
                                            ▼
                          C ABI 契约 (include/shield/plugin/)
                          abi.h / database.h / (+ document/cache/queue/leaderboard)
                                            │ PluginHost dlopen libshield_*.so
                                            ▼
                          独立插件 .so（各自连接池 + 驱动；host 不链接驱动）
```

- **core 不链接任何数据库驱动**：链接边界是插件 DLL 自身（见顶层 `CMakeLists.txt` 的 `DATABASE BACKEND PLUGINS` 段）。
- **连接池在每个插件内部自治**：`include/shield/plugin/database.h` 注释明确 "The plugin owns its own connection pool"；`pool_size` 配置由插件自己消费（见各插件 `manifest.yaml` 的 `config_schema`）。
- **Lua namespace 由插件自注册**：每个插件实现 `register_lua`（见 `include/shield/plugin/abi.h`），安装 `shield.<namespace>` 表，所以 core 的 `lua_api.cpp` 不含 db/redis。

## 接口分类

接口按**能力类型**划分，多个驱动可实现同一接口（业务侧可经 binding 名互换后端）：

| 接口 | 能力 | 实现插件 | 驱动 |
|------|------|----------|------|
| `shield.database.v1` | SQL（query / execute / 事务） | mysql / postgresql / sqlite | mysqlx / libpq / sqlite3 |
| `shield.document.v1` | 文档 CRUD / 聚合 | mongodb | mongocxx |
| `shield.cache.v1` | KV / hash / TTL / counter | cache.redis | redis++ |
| `shield.queue.v1` | 消息队列 | queue.redis | redis++ |
| `shield.leaderboard.v1` | 排行榜 | leaderboard.redis | redis++ |

C ABI 契约见 `include/shield/plugin/database.h`（SQL）；其余接口的 vtable 头在同目录。每个插件在 `manifest.yaml` 的 `provides.interface` 声明自己实现的接口。

## 连接池与可观测性

池逻辑归插件，带来高内聚与驱动隔离，但代价是池指标分散。`shield.pool.stats.v1` 是一个**可选实现**的观测接口，让 host 统一采集各插件池快照（容量、使用、等待、生命周期、健康），供 ops/metrics 消费。详见 [Plugin Pool Stats](plugin-pool-stats.md)。

## 相关文档

- [Lua API 契约](lua-api.md) —— `shield.<plugin-namespace>(binding)` 各数据插件方法的权威定义。
- [插件系统 v1](plugin-system.md) —— 插件 ABI、instance、get_interface、register_lua 机制。
- [Plugin Pool Stats](plugin-pool-stats.md) —— 连接池统一观测接口。
- [插件参考](plugins/index.md) —— 各数据库/Redis 插件的配置与能力。
