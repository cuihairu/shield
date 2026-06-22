# SQLite

> 嵌入式单文件 SQL 数据库插件，零部署、零外部服务，适合开发、测试和单机存档型游戏场景。

`database.sqlite` 是 Shield 官方提供的 [`shield.database.v1`](/plugin-system#interface-model) 实现之一，基于 [sqlite3](https://www.sqlite.org/) C API。它把数据库直接嵌进进程，没有独立的数据库服务，也不需要网络端口。对游戏服来说，典型用途是单服存档、配置表缓存、自动化测试 mock。

## 包信息

- **包 ID**: `database.sqlite`
- **接口**: [`shield.database.v1`](/plugin-system#interface-model)
- **Capabilities**: `sql`, `transactions`
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_DB_PLUGIN_SQLITE`
- **源码**: `plugins/sqlite/`
- **依赖**: [sqlite3](https://www.sqlite.org/) （vcpkg 端口 `sqlite3`）

## 构建启用

在 CMake 配置阶段打开 `SHIELD_BUILD_DB_PLUGIN_SQLITE`：

```bash
cmake -B build -DSHIELD_BUILD_DB_PLUGIN_SQLITE=ON
cmake --build build
```

该选项会触发两件事：

1. `plugins/sqlite/` 下的 shared library 被构建，输出到 `plugins/database.sqlite/bin/`。
2. CMake 把 vcpkg manifest feature `database-sqlite` 透传给 vcpkg，自动安装 `sqlite3` 端口（含头文件和导入库）。

如果只跑纯 C++ 测试、不需要 Lua runtime，可以同时关掉 Lua 相关 feature 以加快构建。

## 配置 Schema

`plugin.json` 中声明的 `config_schema` 只暴露两个字段，SQLite 不需要 host/port/user/password。

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `database` | string | 否 | `:memory:` | 数据库文件路径。`:memory:` 表示纯内存数据库（进程退出即销毁）；填普通路径则落盘。支持 URI 形式（如 `file:test.db?mode=ro`），因为连接时启用了 `SQLITE_OPEN_URI`。 |
| `query_timeout_ms` | integer | 否 | `5000` | 单条 SQL 的 busy timeout，对应 `sqlite3_busy_timeout`。单位毫秒，范围 1-300000。遇到 `SQLITE_BUSY` 时驱动会自动重试到超时。 |

### 完整 app.yaml 示例

一个游戏服场景，主库存档到磁盘文件，另一个内存实例用作热数据缓存：

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: db.main
      package: database.sqlite
      required: true
      config:
        database: "data/game.db"
        query_timeout_ms: 5000
    - id: db.cache
      package: database.sqlite
      required: true
      config:
        database: ":memory:"
  bindings:
    database.default: db.main
    database.cache: db.cache
```

业务代码通过 binding 名 `database.default` 拿到主库，通过 `database.cache` 拿到内存库。两个实例彼此独立，不共享连接池。

## 接口契约

本插件实现 [`include/shield/plugin/database.h`](https://github.com/cuihairu/shield/blob/main/include/shield/plugin/database.h) 定义的 `shield_database_v1` 结构体。所有方法都是 C 函数指针，由 host 通过 `instance->get_interface("shield.database.v1")` 取得。

### 连接生命周期

```c
struct shield_db_conn* (*connect)(const struct shield_db_connect_args* args,
                                  char* err_buf, int err_buf_size);
void (*disconnect)(struct shield_db_conn* conn);
int  (*ping)(struct shield_db_conn* conn);
```

| 方法 | 语义 |
|------|------|
| `connect` | 调用 `sqlite3_open_v2` 打开数据库。flags 固定为 `READWRITE | CREATE | URI | NOMUTEX`。成功返回 `shield_db_conn*`，失败返回 `NULL` 并把错误信息写入 `err_buf`。连接建立后立刻 `sqlite3_busy_timeout(query_timeout_ms)`。 |
| `disconnect` | 调用 `sqlite3_close` 关闭句柄并释放 `shield_db_conn`。`NULL` 安全。 |
| `ping` | 调用 `sqlite3_db_readonly` 探测句柄是否仍可用，返回 1 表示存活。 |

`shield_db_connect_args` 中的 `host`、`port`、`user`、`password` 在 SQLite 侧被忽略；只有 `database`（路径）、`query_timeout_ms` 生效。

### query vs execute

```c
int (*query)(struct shield_db_conn* conn, const char* sql,
             const char* const* params, int n_params,
             struct shield_db_result* out_result);
int (*execute)(struct shield_db_conn* conn, const char* sql,
               const char* const* params, int n_params,
               struct shield_db_result* out_result);
```

SQLite 本身不区分 `SELECT` 与 DML，所以这两个方法在实现里走同一条 prepared statement 路径。两者都：

- 用 `sqlite3_prepare_v2` + `sqlite3_bind_text`/`sqlite3_bind_null` 绑定参数。
- 返回 0 表示调用成功（即使 SQL 层失败，`out_result->success` 可能仍为 0）。
- 返回非 0 表示参数非法或硬错误，host 会把该连接标记为 poison。

约定上的差异：业务代码应该用 `query` 调 `SELECT`，用 `execute` 调 `INSERT/UPDATE/DELETE`。`out_result->affected_rows` 来自 `sqlite3_changes`，`last_insert_id` 来自 `sqlite3_last_insert_rowid`。

### 事务

```c
int (*begin)(struct shield_db_conn* conn, struct shield_db_result* out_result);
int (*commit)(struct shield_db_conn* conn, struct shield_db_result* out_result);
int (*rollback)(struct shield_db_conn* conn, struct shield_db_result* out_result);
```

分别执行 `BEGIN` / `COMMIT` / `ROLLBACK`。SQLite 默认是自动提交模式，调用 `begin` 后进入显式事务。**不暴露** `SAVEPOINT`，嵌套事务由调用方自行管理。

### shield_db_result 的内存所有权

```c
struct shield_db_result {
    int         success;
    const char* error_msg;
    const char* error_code;
    int64_t     affected_rows;
    int64_t     last_insert_id;
    int          row_count;
    int          col_count;
    const char** cells;
};

void (*free_result)(struct shield_db_result* result);
```

**关键规则**：

- `cells` 数组由插件 `malloc` 分配，长度为 `row_count * col_count`，行主序。
- 每个 cell 字符串也是 `malloc` 出来的副本；`NULL` 指针表示 SQL `NULL`，空字符串 `""` 与 `NULL` 是不同语义。
- host 使用完 result 后**必须**调用 `free_result`，由插件释放内部所有 `malloc` 内存。`free_result` 不释放 `result` 结构体本身（通常由 host 在栈上分配）。
- `error_msg` / `error_code` 同样由插件分配，`free_result` 会一并释放。

## 使用示例

### C++ 侧（通过 binding 访问）

```cpp
#include "shield/plugin/database.h"
#include "shield/plugin/plugin_host.hpp"

// 通过 binding 名拿到 shield_database_v1* （所有实例共享同一个静态 vtable）
auto* db = shield::plugin::global_host()
              .get_by_binding<shield_database_v1>("database.default");
if (!db) {
    // 没有配置 database.default binding，或目标实例未启动
    return;
}

// 建立一条新连接（host 的 DatabasePool 通常会池化）
char err_buf[256] = {};
shield_db_connect_args args{};
args.database = "data/game.db";
args.query_timeout_ms = 5000;
shield_db_conn* conn = db->connect(&args, err_buf, sizeof(err_buf));
if (!conn) {
    shield::log::error(std::format("sqlite connect failed: {}", err_buf));
    return;
}

shield_db_result result{};
const char* params[] = { player_id.c_str() };
if (db->query(conn,
              "SELECT player_id, nickname, level FROM players WHERE player_id = ?",
              params, 1, &result) == 0 && result.success) {
    for (int r = 0; r < result.row_count; ++r) {
        const char* pid    = result.cells[r * result.col_count + 0];
        const char* name   = result.cells[r * result.col_count + 1];
        const char* level  = result.cells[r * result.col_count + 2];
        // 业务处理 ...
    }
}
db->free_result(&result);
db->disconnect(conn);
```

### Lua 侧

SQLite 插件通过 `register_lua` 暴露 `shield.database.sqlite` callable namespace：

```lua
local db = shield.database.sqlite("db.main")   -- 通过实例 ID
local ok, row = db:query_one(
    "SELECT player_id, nickname FROM players WHERE player_id = ?",
    { player_id })
```

具体 API 契约见 [Lua API](/lua-api)。插件 Lua proxy 支持 `query`、`query_one`、`execute`、`transaction`。

## 平台特性

### `:memory:` 内存模式

`database: ":memory:"` 创建一个纯内存数据库，进程退出后数据丢失。适合：

- 自动化测试：每个 test case 起一份干净 schema，互不干扰。
- 热数据缓存：把查询结果物化到本地，配合索引加速点查。

`:memory:` 实例之间彼此隔离——SQLite 的内存数据库按连接隔离，host 的连接池会在首次连接时建立 schema，后续复用该连接。

### busy_timeout 与并发写

SQLite 在 WAL 模式下支持多读单写。当写冲突发生时，驱动会按 `query_timeout_ms` 持续重试直到成功或超时。超时后返回 `connection_timeout` 错误码。

业务侧应当：

- 单进程写，避免多连接并发写同一数据库文件。
- 长事务期间不要并发起 `query`，避免触发 `SQLITE_BUSY`。
- 考虑在 schema 里 `PRAGMA journal_mode=WAL;` 提升并发读性能（SQLite 插件本身不强制设置）。

### 只读模式

通过 URI 形式打开只读数据库：

```yaml
config:
  database: "file:data/game.db?mode=ro"
```

插件连接时启用了 `SQLITE_OPEN_URI`，URI 参数由 sqlite3 解析。

### 跨进程访问

SQLite 支持多进程访问同一个文件，但需要正确的锁机制。如果业务在 Shield 进程之外通过其他工具读写同一文件，建议启用 WAL 模式并保证所有进程使用相同的 `busy_timeout`。

## 错误处理

`shield_db_result.success` 为 0 表示 SQL 层失败，`error_code` 是稳定字符串。返回值非 0 表示硬错误（连接断开、参数非法）。

| `error_code` | 触发条件 | 对应 SQLite 结果码 |
|--------------|----------|--------------------|
| `connection_timeout` | 写冲突重试超时 | `SQLITE_BUSY` |
| `constraint_violation` | 唯一索引冲突、外键约束、CHECK 失败 | `SQLITE_CONSTRAINT` |
| `syntax_error` | SQL 语法错误 | `SQLITE_MISMATCH` |
| `auth_failed` | 只读库尝试写、权限不足 | `SQLITE_READONLY`, `SQLITE_PERM` |
| `pool_exhausted` | 内存分配失败 | `SQLITE_NOMEM` |
| `db_query_failed` | 其他查询失败（含文件损坏、非数据库文件） | `SQLITE_CORRUPT`, `SQLITE_NOTADB`, 兜底 |

业务侧应当区分两类错误：

- **可重试**：`connection_timeout`、`pool_exhausted`。
- **不可重试**：`syntax_error`、`constraint_violation`、`auth_failed`。

错误处理示例：

```cpp
shield_db_result r{};
db->execute(conn, "INSERT INTO players(player_id) VALUES(?)", params, 1, &r);
if (!r.success) {
    if (std::string(r.error_code) == "constraint_violation") {
        // 主键冲突，走 upsert 分支
    } else {
        shield::log::error(std::format("sqlite: {} ({})", r.error_msg, r.error_code));
    }
}
db->free_result(&r);
```

## 部署

### 二进制位置

构建产物位于插件包目录：

```
plugins/database.sqlite/
├── plugin.json
└── bin/
    ├── libshield_database_sqlite.dll     # Windows
    ├── libshield_database_sqlite.so      # Linux
    └── libshield_database_sqlite.dylib   # macOS
```

### 运行时依赖

| 平台 | 依赖 |
|------|------|
| Windows | `sqlite3.dll` （由 vcpkg 安装到运行时路径） |
| Linux | `libsqlite3.so.0` （系统包或 vcpkg 安装） |
| macOS | `libsqlite3.dylib` （系统自带，或 vcpkg 版本） |

host 启动时会 `dlopen` / `LoadLibrary` 加载插件 shared library；sqlite3 的导入库必须在动态链接器搜索路径上。vcpkg manifest mode 会把运行时 DLL 复制到可执行目录，CMake 安装目标也会处理这一步。

### 跨平台注意事项

- Windows 下不要把 `.db` 文件放在 OneDrive 等同步目录，SQLite 的文件锁与同步软件不兼容。
- 移动端/嵌入式平台（如 Android）需要确保 sqlite3 编译时启用了 `SQLITE_ENABLE_FTS5` 等扩展（默认 vcpkg 端口已包含常用扩展）。
- macOS 系统自带的 sqlite3 版本较旧，建议使用 vcpkg 版本以获得最新修复。

## 相关链接

- [插件系统](/plugin-system) — Shield 插件 v1 设计、ABI 契约、bootstrap pipeline
- [Shield 数据语义](/runtime-data) — `shield.db.*` Lua API、连接池配置、事务规则
- [SQLite 官方文档](https://www.sqlite.org/docs.html)
- [SQLite WAL 模式](https://www.sqlite.org/wal.html)
- [SQLite URI 语法](https://www.sqlite.org/uri.html)
