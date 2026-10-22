# Data Access Architecture

Shield's data access (SQL, documents, KV, queues, leaderboards) uses a **pure plugin architecture**: the core runtime contains no database code, and all DB/Redis capabilities are provided by separate plugins. This document is the data topic overview; for the authoritative contract see the Lua API and Plugin System v1.

## Historical note: the shield_data plan is obsolete

A `shield_data` core module was once planned (providing `DatabasePool` / `DatabaseConnection` and the `shield.db.*` / `shield.redis.*` Lua APIs), but **that plan never landed**:

- There is no `data` subdirectory under `include/shield/`, and `CMakeLists.txt` has no `shield_data` target (it only appears in the planning table of the [CMake Refactoring](cmake-refactor.md)).
- `src/lua/lua_api.cpp` registers no db/redis modules.
- The `shield_data` / `DatabasePool` / `shield.db:*` / `shield.redis:*` / "already covered by CTest" content described in earlier versions of this document was fictional and is obsolete.

The real shape of data access is described below. On conflict with the Lua API Contract, lua-api.md prevails — it already lists `shield.db:*` / `shield.redis:*` under "deleted legacy APIs". Data access is called per plugin namespace, in the shape `shield.<plugin-namespace>(binding)`: `binding` is the **logical name** declared in configuration (`plugins.bindings`), which the host resolves internally to a plugin instance; the same binding name must be used consistently in configuration and Lua calls. For concrete method names, the Lua API Contract prevails. The pseudocode below is illustrative (`<method>` stands for each plugin's methods; it is not executable Lua):

```text
shield.database.mysql("database.default"):<method>(...)      -- SQL (mysql/postgresql share the same shape)
shield.database.mongodb("document.default"):<method>(...)    -- documents
shield.cache.redis("cache.session"):<method>(...)            -- KV cache
shield.queue.redis("queue.events"):<method>(...)             -- queues
shield.leaderboard.redis("leaderboard.arena"):<method>(...)  -- leaderboards
```

For the design rationale of bindings over instance_id and the naming rules, see Plugin System · Why bindings.

## Architecture: zero data in core + plugin autonomy

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

- **core links no database driver**: the link boundary is the plugin DLL itself (see the `DATABASE BACKEND PLUGINS` section of the top-level `CMakeLists.txt`).
- **Connection pools are autonomous inside each plugin**: the comment in `include/shield/plugin/database.h` states explicitly "The plugin owns its own connection pool"; the `pool_size` config is consumed by the plugin itself (see each plugin's `config_schema` in its `manifest.yaml`).
- **Lua namespaces are self-registered by plugins**: each plugin implements `register_lua` (see `include/shield/plugin/abi.h`) and installs the `shield.<namespace>` table, so core's `lua_api.cpp` contains no db/redis.

## Interface Categories

Interfaces are divided by **capability type**; multiple drivers can implement the same interface (business code can swap backends via binding names):

| Interface | Capability | Implementing plugins | Driver |
|------|------|----------|------|
| `shield.database.v1` | SQL (query / execute / transactions) | mysql / postgresql / sqlite | libmariadb / libpq / sqlite3 |
| `shield.document.v1` | document CRUD / aggregation | mongodb | mongocxx |
| `shield.cache.v1` | KV / hash / TTL / counter | cache.redis | redis++ |
| `shield.queue.v1` | message queues | queue.redis | redis++ |
| `shield.leaderboard.v1` | leaderboards | leaderboard.redis | redis++ |

For the C ABI contract see `include/shield/plugin/database.h` (SQL); the vtable headers of the other interfaces are in the same directory. Each plugin declares the interfaces it implements in its `manifest.yaml` under `provides.interface`.

## Asynchronous Execution Model

The SQL trio (mysql / postgresql / sqlite) `query` / `query_one` / `execute` / `transaction` are **async by default inside coroutine dispatch**: the call suspends the current coroutine, the SQL (including connection pool acquire) runs on the plugin's worker threads, the service keeps processing other messages, and the actor is not blocked. Non-coroutine contexts or an instance configured with `async: false` take the synchronous fallback (behavior identical to before this landed). For the authoritative contract (timeouts, error shapes, transaction body rules) see the Lua API and [DB Async Entry Points](db-async-design.md); for usage discipline see [DB Usage Discipline](db-discipline.md).

## Connection Pools and Observability

Pool logic belonging to plugins brings high cohesion and driver isolation, at the cost of scattered pool metrics. `shield.pool.stats.v1` is an **optionally implemented** observability interface letting the host uniformly collect pool snapshots from each plugin (capacity, usage, waiting, lifecycle, health); `/ops/metrics` already consumes it (`shield_plugin_pool_*`). The async entry points additionally append two gauges through its tail: `shield_plugin_db_pending_async` (in-flight async calls) and `shield_plugin_db_holding` (connections held by open transactions), reported with real values by all three drivers mysql / postgresql / sqlite. See Plugin Pool Stats for details.

## Related Documents

- Lua API Contract — the authoritative definitions of each data plugin's methods under `shield.<plugin-namespace>(binding)`.
- Plugin System v1 — the plugin ABI, instances, get_interface, and the register_lua mechanism.
- Plugin Pool Stats — the unified connection pool observability interface.
- [DB Async Entry Points](db-async-design.md) — coroutine-resume async entry points (landed).
- Plugin Reference — configuration and capabilities of each database/Redis plugin.
