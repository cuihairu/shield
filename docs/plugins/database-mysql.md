# MySQL

> 通过经典 MySQL 协议提供客户端-服务器模式的 SQL 数据库，适合需要持久化、高并发写、跨服共享数据的游戏后端。

`database.mysql` 是 Shield 官方提供的 [`shield.database.v1`](/plugin-system#interface-model) 实现之一，基于 [MariaDB Connector/C](https://mariadb-corporation.github.io/mariadb-connector-c/)（libmariadb）——一个轻量的 C 语言 MySQL 协议客户端，可同时连接 MySQL 与 MariaDB 服务器。所有 SQL 通过二进制协议的 prepared statement 执行，参数与结果均为类型化绑定。

## 包信息

- **包 ID**: `database.mysql`
- **接口**: [`shield.database.v1`](/plugin-system#interface-model)、[`shield.pool.stats.v1`](/plugin-pool-stats)
- **Capabilities**: `sql`, `transactions`
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_DB_PLUGIN_MYSQL`
- **源码**: `plugins/mysql/`
- **依赖**: [libmariadb](https://mariadb-corporation.github.io/mariadb-connector-c/)（vcpkg 端口 `libmariadb`，LGPL-2.1+，传递依赖仅 zlib/openssl）

## 构建启用

在 CMake 配置阶段打开 `SHIELD_BUILD_DB_PLUGIN_MYSQL`：

```bash
cmake -B build -DSHIELD_BUILD_DB_PLUGIN_MYSQL=ON
cmake --build build
```

该选项触发：

1. `plugins/mysql/` 下的 shared library 被构建到 `plugins/database.mysql/bin/`。
2. vcpkg manifest feature `database-mysql` 被启用，自动安装 `libmariadb` 端口（头文件 `<mysql/mysql.h>` 和导入库）。

libmariadb 依赖树很小（zlib + openssl），构建与 CI 时长远轻于 mysql-connector-cpp 一类的重型驱动。

## 配置 Schema

`manifest.yaml` 的 `config_schema` 字段如下。

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `host` | string | 否 | `127.0.0.1` | MySQL 服务器主机名或 IP。 |
| `port` | integer | 否 | `3306` | 经典 MySQL 协议端口。 |
| `database` | string | 是 | — | 默认 schema 名。 |
| `username` | string | 是 | — | 登录用户名。 |
| `user` | string | 否 | — | `username` 的别名（两者同时配置时 `user` 生效）。新配置请统一用 `username`。 |
| `password` | string | 否 | — | 登录密码。标记为 `secret`，日志和 dashboard 会脱敏。 |
| `connect_timeout_ms` | integer | 否 | `5000` | 建立 TCP 连接 + 协议握手的超时，单位毫秒，范围 100-60000。 |
| `query_timeout_ms` | integer | 否 | `5000` | 单条 SQL 执行超时，单位毫秒，范围 100-300000。 |
| `pool_size` | integer | 否 | `4` | 每实例连接池容量，范围 1-64。 |
| `acquire_timeout_ms` | integer | 否 | `10000` | 池耗尽时等待归还的时长，单位毫秒，范围 100-120000。 |
| `async` | boolean | 否 | `true` | `query` / `query_one` / `execute` / `transaction` 走实例 worker 池异步执行（连接 acquire 与 BEGIN 也在 worker 上）：协程内调用挂起等待完成，同 service 其他消息继续处理；事务 body 内的语句挂在被持有的那条连接上，跑在事务专用串行 lane。设为 `false` 退回同步入口（调用即阻塞到 SQL 结束）。见 [DB 异步入口](/db-async-design)。 |
| `call_timeout_ms` | integer | 否 | `0` | 异步入口的调用方挂起预算。`0` 表示 `query_timeout_ms + 500`。超时返回 `{code="timeout", retryable=true}`；迟到的真实结果会被丢弃（不投递），不会覆盖超时结果。范围 0-300000。 |

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
        port: 3306
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
        port: 3306
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
| `connect` | `mysql_init` + `mysql_real_connect`（带 connect/read/write 超时与 `utf8mb4` 字符集）。失败时把 `mysql_error` 写入 `err_buf`，返回 `NULL`。 |
| `disconnect` | `mysql_close` + 释放内部结构。`NULL` 安全，幂等。 |
| `ping` | `mysql_ping`，成功返回 1，任何错误返回 0。 |

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

两者底层共用 `run_stmt`（prepared statement 引擎），差别在 `collect_rows` 标志：

| 方法 | 行为 |
|------|------|
| `query` | `collect_rows=true`：`mysql_stmt_fetch` 循环把所有行物化到 `out_result->cells`。适合 `SELECT`。 |
| `execute` | `collect_rows=false`：只取 `mysql_stmt_affected_rows` 和 `mysql_stmt_insert_id`。适合 `INSERT/UPDATE/DELETE`。 |

参数绑定：vtable 路径所有参数按文本（`MYSQL_TYPE_STRING`）绑定；`NULL` 参数（`params[i] == NULL`）绑定为真正的 SQL `NULL`（`MYSQL_TYPE_NULL`）。Lua 路径则按 Lua 类型分别绑定整数/浮点/字符串/NULL。

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
args.port = 3306;
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

### Lua 侧

MySQL 插件通过 `register_lua` 暴露 `shield.database.mysql` callable namespace：

```lua
local db = shield.database.mysql("database.default")
local ok, rows = db:query(
    "SELECT player_id, nickname FROM players WHERE level > ?",
    { 10 })
local ok, err = db:transaction(function(tx)
    local ok2, r = tx:execute(
        "UPDATE wallet SET gold = gold - ? WHERE player_id = ?",
        { amount, pid })
    if not ok2 then return false, r end  -- 首返回值 false → ROLLBACK
    tx:execute("INSERT INTO logs(uid, action) VALUES(?, ?)",
               { pid, "debit" })
    return true, r.affected              -- → COMMIT，值透传给外层
end)
```

具体 API 契约见 [Lua API](/lua-api) 的 Database（SQL）一节。协程派发内这些调用默认异步挂起（`async` 开关），返回形状与同步路径一致。

## 平台特性

### 协议选择：经典 MySQL 协议

插件基于 **libmariadb 的经典 MySQL 协议客户端**（`mysql_*` / `mysql_stmt_*` C API，默认端口 3306），不是 X DevAPI / X Protocol：

- 经典协议同时被 MySQL 与 MariaDB 服务器支持，一份客户端两种服务端。
- 所有 SQL 走二进制协议的 prepared statement（`mysql_stmt_*`），`?` 参数是绑定进去的，不做字符串拼接。
- 不需要在 `my.cnf` 启用 `mysqlx` 插件，也不需要 X Protocol 权限；打通 3306（或实例配置的 `port`）即可。

### 连接字符集

插件在建连时强制客户端字符集 `utf8mb4`（`MYSQL_SET_CHARSET_NAME`）。建议 MySQL 服务端也配置 `utf8mb4` 作为默认字符集：

```ini
[mysqld]
character-set-server = utf8mb4
collation-server = utf8mb4_unicode_ci
```

插件不暴露客户端字符集覆盖选项——业务侧如果需要特殊字符集，可以通过 `SET NAMES` 显式设置。

### SSL 选项

libmariadb 支持 `MYSQL_OPT_SSL_*` 系列选项，但当前插件实例 config 未暴露 SSL 字段（v1 ABI 的 `shield_db_connect_args.extra_json` 也不解析）。需要加密链路时，现阶段可在服务端前置 TLS 代理（如 stunnel / 云厂 SSL 终结），或等后续把 SSL 选项加进实例 config。

### 连接池与 worker 池

Lua 路径（`shield.database.mysql(binding)` 的 proxy 方法）的连接池在**插件实例内部自治**：free-list + `pool_size`（容量）+ `acquire_timeout_ms`（耗尽时等待归还的时长），坏连接用完即弃、由下次 acquire 补新。`async` 开启时另有一组 worker 线程（数量 = min(`pool_size`, 8)，首用懒建）承接 acquire 与语句执行；异步事务的语句/提交/回滚跑在单条串行 tx lane 上。C vtable 路径仍是 per-call `connect`/`disconnect`，不走池也不走 worker。

池与异步入口的实时指标经 [`shield.pool.stats.v1`](/plugin-pool-stats) 上报，`/ops/metrics` 导出 `shield_plugin_pool_*`（容量/使用/空闲）与 `shield_plugin_db_pending_async` / `shield_plugin_db_holding`。

## 错误处理

`shield_db_result.success` 为 0 时，`error_code` 由 `map_mysql_error` 映射：**优先按数值错误码**（服务端 MySQL/MariaDB errno，如 1062/1213；客户端 `CR_*`，如 `CR_SERVER_GONE_ERROR`），数值码未覆盖时按错误消息关键字兜底匹配。

| `error_code` | 触发条件 | 错误码 / 关键字 |
|--------------|----------|----------------|
| `connection_lost` | 连接断开、服务器宕机 | `Lost connection`, `server has gone away` |
| `connection_timeout` | 查询或连接超时 | `timeout`, `timed out` |
| `syntax_error` | SQL 语法错误 | `syntax`, `SQL syntax` |
| `constraint_violation` | 主键冲突、外键、CHECK | `Duplicate`, `foreign key`, `constraint` |
| `transaction_aborted` | 死锁 | `Deadlock` |
| `db_query_failed` | 兜底 | 其他所有驱动错误（未命中以上规则） |

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
├── manifest.yaml
└── bin/
    ├── libshield_db_mysql.dll        # Windows
    ├── libshield_db_mysql.so         # Linux
    └── libshield_db_mysql.dylib      # macOS
```

### 运行时依赖

| 平台 | 驱动形态 |
|------|----------|
| 全平台 | vcpkg 端口 `libmariadb` 以**静态库**链接进插件产物（Linux 为 `libmariadb.a`，`ldd libshield_db_mysql.so` 只剩系统库），运行时不需要随包驱动 DLL/SO |

这是启用 `database-mysql` feature 时 vcpkg manifest mode 自动处理的：构建期解析依赖、链接进 `.so`/`.dll`，部署目录里只有插件本身。

### 跨平台注意事项

- 驱动统一走 vcpkg 构建，不要用系统包与 vcpkg 版本混链（静态链接下不存在运行时抢注，但构建期头/库不一致会出 ABI 问题）。
- 容器化部署时，确保 MySQL/MariaDB 服务端经典协议端口（默认 3306，实例 `port` 配置）已在 firewall/security group 放行。
- 服务端账号按 MySQL 与 MariaDB 任一发行版授权即可，插件对两者都兼容。

## 相关链接

- [插件系统](/plugin-system) — Shield 插件 v1 设计、ABI 契约
- [Shield 数据语义](/runtime-data) — 连接池配置、事务规则、错误处理
- [DB 使用纪律](/db-discipline) — 超时、池匹配、事务 body 规则
- [MariaDB Connector/C 文档](https://mariadb-corporation.github.io/mariadb-connector-c/)
- [MySQL 服务端错误码参考](https://dev.mysql.com/doc/mysql-errors/8.0/en/server-error-reference.html)
