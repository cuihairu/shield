# MySQL

> 通过 MySQL X DevAPI 提供客户端-服务器模式的 SQL 数据库，适合需要持久化、高并发写、跨服共享数据的游戏后端。

`database.mysql` 是 Shield 官方提供的 [`shield.database.v1`](/plugin-system#interface-model) 实现之一，基于 [MySQL Connector/C++](https://dev.mysql.com/doc/connector-cpp/en/) X DevAPI。X DevAPI 默认走 MySQL X Protocol（端口 33060），而非经典协议（端口 3306）——业务方部署 MySQL 时需要启用 `mysqlx` 插件并开放 33060 端口。

## 包信息

- **包 ID**: `database.mysql`
- **接口**: [`shield.database.v1`](/plugin-system#interface-model)
- **Capabilities**: `sql`, `transactions`
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_DB_PLUGIN_MYSQL`
- **源码**: `plugins/mysql/`
- **依赖**: [mysql-connector-cpp](https://dev.mysql.com/doc/connector-cpp/en/) （vcpkg 端口 `mysql-connector-cpp`，提供 X DevAPI）

## 构建启用

在 CMake 配置阶段打开 `SHIELD_BUILD_DB_PLUGIN_MYSQL`：

```bash
cmake -B build -DSHIELD_BUILD_DB_PLUGIN_MYSQL=ON
cmake --build build
```

该选项触发：

1. `plugins/mysql/` 下的 shared library 被构建到 `plugins/database.mysql/bin/`。
2. vcpkg manifest feature `database-mysql` 被启用，自动安装 `mysql-connector-cpp` 端口（X DevAPI 头文件 `<mysqlx/xdevapi.h>` 和导入库）。

`mysql-connector-cpp` 是一个较大的依赖（含 protobuf、icu 等），首次 vcpkg 安装耗时较长。建议本地开发时只在需要 MySQL 的 feature set 下开启。

## 配置 Schema

`plugin.json` 的 `config_schema` 字段如下。

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `host` | string | 否 | `127.0.0.1` | MySQL 服务器主机名或 IP。 |
| `port` | integer | 否 | `3306` | **经典协议端口**。注意：X DevAPI 实际使用 X Protocol 端口（默认 33060），但插件当前直接把此端口透传给 `mysqlx::Session`；生产环境部署需要确认 MySQL 实例的 `mysqlx_port` 配置。 |
| `database` | string | 是 | — | 默认 schema 名。 |
| `username` | string | 是 | — | 登录用户名。 |
| `password` | string | 否 | — | 登录密码。标记为 `secret`，日志和 dashboard 会脱敏。 |
| `connect_timeout_ms` | integer | 否 | `5000` | 建立 TCP 连接 + X Protocol 握手的超时，单位毫秒，范围 100-60000。 |
| `query_timeout_ms` | integer | 否 | `5000` | 单条 SQL 执行超时，单位毫秒，范围 100-300000。 |

### 完整 app.yaml 示例

一个游戏服场景，主业务库 + 审计库双实例：

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: db.main
      package: database.mysql
      required: true
      config:
        host: "10.0.0.10"
        port: 33060
        database: "game"
        username: "shield_app"
        password: "${DB_MAIN_PASSWORD}"
        connect_timeout_ms: 5000
        query_timeout_ms: 30000
    - id: db.audit
      package: database.mysql
      required: false
      config:
        host: "10.0.0.11"
        port: 33060
        database: "audit"
        username: "shield_audit"
        password: "${DB_AUDIT_PASSWORD}"
  bindings:
    database.default: db.main
    database.audit: db.audit
```

`db.audit` 设为 `required: false`，审计库故障不会阻塞主业务启动；调用方需要在拿到 `NULL` vtable 时降级处理。

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
| `connect` | 构造一个 `mysqlx::Session(host, port, user, password, database)`。失败时捕获 `mysqlx::Error` / `std::exception` 写入 `err_buf`，返回 `NULL`。 |
| `disconnect` | 先 `session->close()`，再 `delete` 内部结构。`NULL` 安全。`close()` 内部异常被吞掉（保证幂等）。 |
| `ping` | 执行 `SELECT 1`，成功返回 1，任何异常返回 0。 |

`shield_db_connect_args` 中所有字段都参与连接：`host`、`port`、`user`、`password`、`database`。`extra_json` 当前不解析。

### query vs execute

```c
int (*query)(struct shield_db_conn* conn, const char* sql,
             const char* const* params, int n_params,
             struct shield_db_result* out_result);
int (*execute)(struct shield_db_conn* conn, const char* sql,
               const char* const* params, int n_params,
               struct shield_db_result* out_result);
```

两者底层共用 `run_stmt`，差别在 `collect_rows` 标志：

| 方法 | 行为 |
|------|------|
| `query` | `collect_rows=true`：迭代 result set 把所有行物化到 `out_result->cells`。适合 `SELECT`。 |
| `execute` | `collect_rows=false`：只取 `getAffectedItemsCount()` 和 `getAutoIncrementValue()`。适合 `INSERT/UPDATE/DELETE`。 |

参数绑定：`stmt.bind(params[i])`。`NULL` 参数（`params[i] == NULL`）会被绑定成空字符串 `""`——这是 mysqlx API 限制，业务侧如果需要真正的 SQL NULL，需要改用专门接口（当前 v1 ABI 暂未暴露）。

返回值规则同 SQLite：0 表示调用成功（SQL 层失败由 `out_result->success=0` 表达），非 0 表示硬错误。

### 事务

```c
int (*begin)(struct shield_db_conn* conn, struct shield_db_result* out_result);
int (*commit)(struct shield_db_conn* conn, struct shield_db_result* out_result);
int (*rollback)(struct shield_db_conn* conn, struct shield_db_result* out_result);
```

分别执行 `START TRANSACTION` / `COMMIT` / `ROLLBACK`。**不暴露** `SAVEPOINT`，也不自动重试死锁——死锁会以 `transaction_aborted` 错误码返回，业务侧自行决定是否重试。

### shield_db_result 的内存所有权

同 [SQLite 文档](/plugins/database-sqlite#shield-db-result-的内存所有权) 的规则：

- `cells` 数组及其中每个字符串都由插件 `malloc` 分配。
- host 必须在使用完后调用 `free_result`。
- `error_msg` / `error_code` 一并在 `free_result` 中释放。
- `free_result` 不释放 `result` 结构体本身。

## 使用示例

### C++ 侧（通过 binding 访问）

```cpp
#include "shield/plugin/database.h"
#include "shield/plugin/plugin_host.hpp"

auto* db = shield::plugin::global_host()
              .get_by_binding<shield_database_v1>("database.default");
if (!db) return;

shield_db_connect_args args{};
args.host = "10.0.0.10";
args.port = 33060;
args.user = "shield_app";
args.password = std::getenv("DB_MAIN_PASSWORD");
args.database = "game";
args.connect_timeout_ms = 5000;
args.query_timeout_ms = 30000;

char err_buf[256] = {};
shield_db_conn* conn = db->connect(&args, err_buf, sizeof(err_buf));
if (!conn) {
    shield::log::error(std::format("mysql connect failed: {}", err_buf));
    return;
}

// 事务扣金币
shield_db_result r{};
db->begin(conn, &r);
db->free_result(&r);

const char* params[] = { amount_str.c_str(), player_id.c_str() };
if (db->execute(conn,
                "UPDATE wallet SET gold = gold - ? WHERE player_id = ?",
                params, 2, &r) == 0 && r.success) {
    shield::log::info(std::format("affected rows: {}", r.affected_rows));
}
db->free_result(&r);

shield_db_result cr{};
db->commit(conn, &cr);
db->free_result(&cr);

db->disconnect(conn);
```

### Lua 侧（规划中）

MySQL 插件的 Lua 绑定在 Lua 自治迁移完成前不暴露。规划中的调用约定：

```lua
local db = shield.database.mysql("db.main")
local ok, rows = db:query(
    "SELECT player_id, nickname FROM players WHERE level > ?",
    { 10 })
local ok, err = db:transaction(function(tx)
    tx:execute("UPDATE wallet SET gold = gold - ? WHERE player_id = ?",
               { amount, pid })
    tx:execute("INSERT INTO logs(uid, action) VALUES(?, ?)",
               { pid, "debit" })
end)
```

具体 API 契约见 [Lua API](/lua-api)。

## 平台特性

### X Protocol vs 经典协议

MySQL Connector/C++ 同时支持经典协议和 X Protocol，但 **Shield 的 MySQL 插件当前只使用 X DevAPI**（即 `mysqlx::Session`），对应 X Protocol（默认端口 33060）。原因：

- X DevAPI 的参数绑定、流式 result set、CRUD API 更现代化。
- `getAutoIncrementValue()` 等 API 直接可用，不需要再发额外 SQL。

业务部署时需要：

- 在 `my.cnf` 启用 `mysqlx` 插件（MySQL 8.0+ 默认启用）。
- 开放 `mysqlx_port`（默认 33060）。
- 用户账号需要有 X Protocol 权限。

### 连接字符集

X DevAPI 在握手阶段协商字符集。建议 MySQL 服务端配置 `utf8mb4` 作为默认字符集：

```ini
[mysqld]
character-set-server = utf8mb4
collation-server = utf8mb4_unicode_ci
```

插件当前不暴露客户端字符集覆盖选项——业务侧如果需要特殊字符集，可以通过 `SET NAMES` 显式设置。

### SSL 选项

当前 v1 ABI 没有暴露 SSL 选项字段。如果业务需要 SSL 连接，建议：

- 在 MySQL 服务端强制 SSL（`REQUIRE SSL`）。
- 等待后续 v1.x 扩展 `shield_db_connect_args.extra_json` 支持 SSL 配置。

### 连接池

`shield.database.v1` 只提供连接工厂，**不实现连接池**。host 的 `shield::data::DatabasePool` 在此 vtable 之上构建池化逻辑（详见 [数据语义](/runtime-data)）。业务侧不应直接持有 `shield_db_conn*` 长期不开释，应当用完即 `disconnect`，让池回收。

## 错误处理

`shield_db_result.success` 为 0 时，`error_code` 来自对 `mysqlx::Error::what()` 字符串的模式匹配。这是 v1 的临时实现，后续可能改为读取 mysqlx 内部错误码做精确映射。

| `error_code` | 触发条件 | 错误消息关键字 |
|--------------|----------|----------------|
| `connection_lost` | 连接断开、服务器宕机 | `Lost connection`, `server has gone away` |
| `connection_timeout` | 查询或连接超时 | `timeout`, `timed out` |
| `syntax_error` | SQL 语法错误 | `syntax`, `SQL syntax` |
| `constraint_violation` | 主键冲突、外键、CHECK | `Duplicate`, `foreign key`, `constraint` |
| `transaction_aborted` | 死锁 | `Deadlock` |
| `db_query_failed` | 兜底 | 其他所有 `mysqlx::Error` |

业务侧重试策略建议：

| 错误码 | 重试 |
|--------|------|
| `connection_lost`, `connection_timeout`, `transaction_aborted` | 可重试（注意 `transaction_aborted` 重试时需要重开事务） |
| `syntax_error`, `constraint_violation` | 不可重试，属于程序 bug |
| `db_query_failed` | 视消息内容判断，默认不重试 |

## 部署

### 二进制位置

```
plugins/database.mysql/
├── plugin.json
└── bin/
    ├── libshield_db_mysql.dll        # Windows
    ├── libshield_db_mysql.so         # Linux
    └── libshield_db_mysql.dylib      # macOS
```

### 运行时依赖

| 平台 | 依赖 |
|------|------|
| Windows | `mysqlcppconn.dll` 及其传递依赖（protobuf、icu、openssl 等） |
| Linux | `libmysqlcppconn.so`、`libprotobuf`、`libicuuc`、`libssl` |
| macOS | `libmysqlcppconn.dylib` 及同样传递依赖 |

vcpkg manifest mode 会自动把这些 DLL/DYLIB 部署到可执行目录。Linux 下建议用 `ldd` 确认链接关系，必要时调整 `LD_LIBRARY_PATH` 或安装到系统路径。

### 跨平台注意事项

- MySQL Connector/C++ 在某些 Linux 发行版上对 OpenSSL 版本敏感，建议统一使用 vcpkg 安装而非系统包。
- Windows 下 vcpkg 构建的 mysql-connector-cpp 可能与官方 MySQL Installer 的运行时不兼容，不要混用。
- 容器化部署时，确保 MySQL 侧的 `mysqlx_port` 已在 firewall/security group 放行。

## 相关链接

- [插件系统](/plugin-system) — Shield 插件 v1 设计、ABI 契约
- [Shield 数据语义](/runtime-data) — 连接池配置、事务规则、错误处理
- [MySQL Connector/C++ 文档](https://dev.mysql.com/doc/connector-cpp/en/)
- [MySQL X DevAPI 用户指南](https://dev.mysql.com/doc/x-devapi-userguide/en/)
- [MySQL X Plugin 配置](https://dev.mysql.com/doc/refman/8.0/en/x-plugin.html)
