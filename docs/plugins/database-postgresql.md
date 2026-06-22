# PostgreSQL

> 通过 libpq 提供企业级关系型数据库访问，适合复杂查询、强一致性约束、JSON/Array 等富类型场景。

`database.postgresql` 是 Shield 官方提供的 [`shield.database.v1`](/plugin-system#interface-model) 实现之一，基于 [libpq](https://www.postgresql.org/docs/current/libpq.html)（PostgreSQL 官方 C 客户端库）。libpq 是 PostgreSQL 协议的参考实现，支持经典协议的所有特性，包括 SSL、SCRAM 认证、异步通知等。

## 包信息

- **包 ID**: `database.postgresql`
- **接口**: [`shield.database.v1`](/plugin-system#interface-model)
- **Capabilities**: `sql`, `transactions`
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_DB_PLUGIN_POSTGRESQL`
- **源码**: `plugins/postgresql/`
- **依赖**: [libpq](https://www.postgresql.org/docs/current/libpq.html) （vcpkg 端口 `libpq`）

## 构建启用

在 CMake 配置阶段打开 `SHIELD_BUILD_DB_PLUGIN_POSTGRESQL`：

```bash
cmake -B build -DSHIELD_BUILD_DB_PLUGIN_POSTGRESQL=ON
cmake --build build
```

该选项触发：

1. `plugins/postgresql/` 下的 shared library 被构建到 `plugins/database.postgresql/bin/`。
2. vcpkg manifest feature `database-postgresql` 被启用，自动安装 `libpq` 端口（含 `libpq-fe.h` 头文件和导入库）。

`libpq` 是一个相对轻量的依赖（远小于 mysql-connector-cpp），构建体验较友好。

## 配置 Schema

`plugin.json` 的 `config_schema` 字段如下。

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `host` | string | 否 | `127.0.0.1` | PostgreSQL 服务器主机名或 IP。 |
| `port` | integer | 否 | `5432` | PostgreSQL 端口。范围 1-65535。 |
| `database` | string | 是 | — | 数据库名。 |
| `username` | string | 是 | — | 登录用户名。 |
| `password` | string | 否 | — | 登录密码。标记为 `secret`，日志和 dashboard 会脱敏。 |
| `connect_timeout_ms` | integer | 否 | `5000` | 连接超时（秒级精度，libpq 的 `connect_timeout` 单位是秒），范围 100-60000。 |
| `query_timeout_ms` | integer | 否 | `5000` | 单条 SQL 超时，范围 100-300000。 |

### 完整 app.yaml 示例

一个游戏服场景，主业务库 + 只读副本：

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: db.main
      package: database.postgresql
      required: true
      config:
        host: "pg-primary.internal"
        port: 5432
        database: "game"
        username: "shield_app"
        password: "${DB_MAIN_PASSWORD}"
        connect_timeout_ms: 5000
        query_timeout_ms: 30000
    - id: db.replica
      package: database.postgresql
      required: false
      config:
        host: "pg-replica.internal"
        port: 5432
        database: "game"
        username: "shield_reader"
        password: "${DB_REPLICA_PASSWORD}"
  bindings:
    database.default: db.main
    database.readonly: db.replica
```

`db.replica` 设为 `required: false`，副本故障不阻塞主业务；调用方需要降级处理。

## 接口契约

本插件实现 [`include/shield/plugin/database.h`](https://github.com/cuihairu/shield/blob/main/include/shield/plugin/database.h) 的 `shield_database_v1`。

### 连接生命周期

```c
struct shield_db_conn* (*connect)(const struct shield_db_connect_args* args,
                                  char* err_buf, int err_buf_size);
void (*disconnect)(struct shield_db_conn* conn);
int  (*ping)(struct shield_db_conn* conn);
```

| 方法 | 语义 |
|------|------|
| `connect` | 把 `shield_db_connect_args` 字段拼成 libpq [connection string](https://www.postgresql.org/docs/current/libpq-connect.html#LIBPQ-CONNSTRING)：`host=... port=... user=... password=... dbname=... connect_timeout=...`，调用 `PQconnectdb`。失败返回 `NULL` 并把 `PQerrorMessage` 写入 `err_buf`。 |
| `disconnect` | 调用 `PQfinish`。`NULL` 安全。 |
| `ping` | 执行 `SELECT 1`，成功返回 1。 |

注意 libpq 的 `connect_timeout` 单位是**秒**（不是毫秒），插件内部把 `connect_timeout_ms` 除以 1000 转换。`query_timeout_ms` 不直接对应 libpq 选项——业务侧需要时可以通过 `SET statement_timeout` 在会话级设置。

### query vs execute

```c
int (*query)(struct shield_db_conn* conn, const char* sql,
             const char* const* params, int n_params,
             struct shield_db_result* out_result);
int (*execute)(struct shield_db_conn* conn, const char* sql,
               const char* const* params, int n_params,
               struct shield_db_result* out_result);
```

两者底层共用 `run_pg_query`，差别在 `is_select` 标志，影响 `PGRES_TUPLES_OK` 的处理方式：

| 方法 | 行为 |
|------|------|
| `query` | `is_select=true`：把 result set 物化到 `out_result->cells`。 |
| `execute` | `is_select=false`：`PGRES_TUPLES_OK` 时也按 command 处理，不读 cells。 |

参数绑定使用 `PQexecParams`（带参数的 prepared execution）。`n_params == 0` 时退回 `PQexec`（普通查询）。

### 占位符重写

**重要**：libpq 要求 SQL 占位符是 `$1, $2, ...` 形式，而 Shield 其他插件（MySQL/SQLite）用 `?`。为了业务代码跨数据库可移植，本插件在执行前做一次**朴素**的占位符重写：扫描 SQL，把每个 `?` 替换成 `$1, $2, ...`。

| 情况 | 处理 |
|------|------|
| 普通 SQL `WHERE id = ?` | 正常重写为 `WHERE id = $1` |
| SQL 字符串字面量中的 `?` `'what?'` | **会错误重写**。当前重写器不区分字符串字面量，会破坏 SQL。 |
| 注释中的 `?` | 同上，也会被错误重写 |

Shield 的 `shield::data` SQL helper 不会生成这种 SQL，所以业务侧使用 mapper/entity helper 时安全。直接拼 SQL 时需要避开在字符串字面量或注释中放 `?`。

### 事务

```c
int (*begin)(struct shield_db_conn* conn, struct shield_db_result* out_result);
int (*commit)(struct shield_db_conn* conn, struct shield_db_result* out_result);
int (*rollback)(struct shield_db_conn* conn, struct shield_db_result* out_result);
```

分别执行 `BEGIN` / `COMMIT` / `ROLLBACK`。**不暴露** `SAVEPOINT`。PostgreSQL 在事务中发生错误后会进入 "aborted transaction" 状态，后续 SQL 全部失败，业务侧必须显式 `ROLLBACK` 后再重试。

### shield_db_result 的内存所有权

同其他数据库插件：

- `cells` 数组及其中每个字符串都由插件 `malloc` 分配，长度 `row_count * col_count`。
- `NULL` 指针表示 SQL `NULL`（通过 `PQgetisnull` 判断）。
- host 必须在使用完后调用 `free_result`；该函数会释放 `cells`、`error_msg`、`error_code`，但不释放 `result` 结构体本身。

## 使用示例

### C++ 侧（通过 binding 访问）

```cpp
#include "shield/plugin/database.h"
#include "shield/plugin/plugin_host.hpp"

auto* db = shield::plugin::global_host()
              .get_by_binding<shield_database_v1>("database.default");
if (!db) return;

shield_db_connect_args args{};
args.host = "pg-primary.internal";
args.port = 5432;
args.user = "shield_app";
args.password = std::getenv("DB_MAIN_PASSWORD");
args.database = "game";
args.connect_timeout_ms = 5000;
args.query_timeout_ms = 30000;

char err_buf[256] = {};
shield_db_conn* conn = db->connect(&args, err_buf, sizeof(err_buf));
if (!conn) {
    shield::log::error(std::format("pg connect failed: {}", err_buf));
    return;
}

// 用 ? 占位符（插件自动重写为 $1, $2 ...）
shield_db_result r{};
const char* params[] = { player_id.c_str() };
if (db->query(conn,
              "SELECT player_id, nickname, level "
              "FROM players WHERE player_id = ?",
              params, 1, &r) == 0 && r.success) {
    for (int i = 0; i < r.row_count; ++i) {
        const char* pid   = r.cells[i * r.col_count + 0];
        const char* name  = r.cells[i * r.col_count + 1];
        const char* level = r.cells[i * r.col_count + 2];
        // 业务处理 ...
    }
}
db->free_result(&r);
db->disconnect(conn);
```

### Lua 侧

PostgreSQL 插件通过 `register_lua` 暴露 `shield.database.postgresql` callable namespace：

```lua
local db = shield.database.postgresql("db.main")
local ok, row = db:query_one(
    "SELECT player_id, nickname FROM players WHERE player_id = ?",
    { player_id })

local ok, err = db:transaction(function(tx)
    tx:execute("UPDATE wallet SET gold = gold - ? WHERE player_id = ?",
               { amount, pid })
    tx:execute("INSERT INTO logs(uid, action) VALUES(?, ?)",
               { pid, "debit" })
end)
```

具体 API 契约见 [Lua API](/lua-api)。

## 平台特性

### libpq 连接字符串

本插件在内部把配置字段拼成 libpq connection string。如果业务需要更复杂的连接选项（如 SSL mode、target_session_attrs、service name），可以等后续扩展 `shield_db_connect_args.extra_json` 来传递——当前 v1 ABI 不暴露该字段。

### LISTEN / NOTIFY（未支持）

PostgreSQL 提供 `LISTEN/NOTIFY` 实现进程间通知，但本插件基于 `PQexec` 同步执行，**没有实现**异步监听。如果业务需要 NOTIFY，可以：

- 单独建一个长连接，用 `PQexec` 发 `LISTEN channel`，然后用业务自己的循环调用 `PQconsumeInput` + `PQnotifies` 轮询。
- 等待后续 v1.x 在 `shield.queue.v1` 之上提供 PostgreSQL NOTIFY 适配。

### SSL

libpq 原生支持 SSL，但当前 v1 ABI 不暴露 SSL 选项字段。默认行为是 libpq 的 `sslmode=prefer`，即服务器支持 SSL 就用，否则明文。如果业务需要强制 SSL，可以通过以下方式之一：

- 在 PostgreSQL 服务端 `pg_hba.conf` 强制 `hostssl`。
- 设置环境变量 `PGSSLMODE=verify-full`（libpq 会读取）。
- 等待 `extra_json` 字段扩展后通过插件配置。

### 错误状态机

PostgreSQL 的事务错误状态机比 MySQL/SQLite 严格：事务中任意一条 SQL 失败，整个事务进入 aborted 状态，后续所有 SQL 直接返回错误，必须 `ROLLBACK` 才能恢复。业务侧使用 `shield.db.transaction` 时，helper 会自动在 callback 返回 `false` 时 rollback，但显式事务场景需要业务方自己注意。

## 错误处理

`shield_db_result.success` 为 0 时，`error_code` 通过 PostgreSQL [SQLSTATE](https://www.postgresql.org/docs/current/errcodes-appendix.html) 前缀映射。

| `error_code` | SQLSTATE 前缀 | 类别 |
|--------------|---------------|------|
| `connection_lost` | `08*` (connection exception), `57*` (operator intervention) | 连接异常、服务器重启 |
| `pool_exhausted` | `53*` (insufficient resources) | 内存/磁盘不足 |
| `transaction_aborted` | `40*` (transaction rollback) | 死锁、序列化失败、事务中断 |
| `constraint_violation` | `23*` (integrity constraint violation) | 主键冲突、外键、CHECK、NOT NULL |
| `syntax_error` | `42*` (syntax error or access rule violation) | SQL 语法错误、表/列不存在 |
| `auth_failed` | `28*` (invalid authorization) | 用户名/密码错误 |
| `db_query_failed` | 其他所有 | 兜底 |

业务侧重试策略：

| 错误码 | 重试 |
|--------|------|
| `connection_lost`, `pool_exhausted` | 可重试，但 `pool_exhausted` 通常表示数据库压力过大，应配合退避 |
| `transaction_aborted` | 可重试（死锁），重试时需要重开事务 |
| `constraint_violation`, `syntax_error`, `auth_failed` | 不可重试，属于程序 bug 或配置错误 |

具体 SQLSTATE 字符串不通过 v1 ABI 暴露——业务侧只能拿到上面的稳定 `error_code`。如果需要精确区分（例如区分 unique_violation 和 foreign_key_violation），可以通过 `error_msg` 字符串匹配 `SQLSTATE "23505"` 等模式。

## 部署

### 二进制位置

```
plugins/database.postgresql/
├── plugin.json
└── bin/
    ├── libshield_db_pgsql.dll        # Windows
    ├── libshield_db_pgsql.so         # Linux
    └── libshield_db_pgsql.dylib      # macOS
```

### 运行时依赖

| 平台 | 依赖 |
|------|------|
| Windows | `libpq.dll` 及其传递依赖（OpenSSL 等） |
| Linux | `libpq.so.5`、`libssl`、`libcrypto` |
| macOS | `libpq.dylib`（可通过 Postgres.app 或 vcpkg 提供） |

Linux 下 `libpq` 通常作为系统包提供（`apt install libpq5` 或 `yum install postgresql-libs`），但建议使用 vcpkg 版本以确保与编译时头文件一致。

### 跨平台注意事项

- Windows 下 PostgreSQL 官方提供的安装包与 vcpkg 构建的 libpq 可能 ABI 不一致，不要混用。
- 容器化部署时，确保 PostgreSQL 客户端证书（如使用 SSL 客户端证书认证）挂载到正确路径，libpq 默认读取 `~/.postgresql/`。
- macOS 上 Postgres.app 自带的 libpq 可以直接使用，但需要把 `/Applications/Postgres.app/Contents/Versions/latest/lib` 加入库搜索路径，建议改用 vcpkg 简化。

## 相关链接

- [插件系统](/plugin-system) — Shield 插件 v1 设计、ABI 契约
- [Shield 数据语义](/runtime-data) — 连接池配置、事务规则、错误处理
- [PostgreSQL libpq 文档](https://www.postgresql.org/docs/current/libpq.html)
- [PostgreSQL 错误码](https://www.postgresql.org/docs/current/errcodes-appendix.html)
- [PostgreSQL 连接字符串语法](https://www.postgresql.org/docs/current/libpq-connect.html#LIBPQ-CONNSTRING)
