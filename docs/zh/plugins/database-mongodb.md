# MongoDB

> 文档型数据库插件，基于 [mongo-cxx-driver](https://mongodb.github.io/mongo-cxx-driver/) 提供 [`shield.document.v1`](/plugin-system#interface-model) 实现，适合玩家背包、社交关系、运行日志、配置化物品等无固定 schema 的业务数据。

`database.mongodb` 与 SQL 系列插件（`database.sqlite` / `database.mysql` / `database.postgresql`）走不同的接口：SQL 系实现 `shield.database.v1`，本插件实现 `shield.document.v1`。两者并列、互不替代。业务侧按数据形态选择：固定表结构走 SQL 接口；文档/嵌套/聚合管道走 document 接口。

## 包信息

- **包 ID**: `database.mongodb`
- **接口**: [`shield.document.v1`](https://github.com/cuihairu/shield/blob/main/include/shield/plugin/document.h)
- **Capabilities**: `crud`、`aggregation`、`transactions`
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_PLUGIN_MONGODB`
- **源码**: `plugins/mongodb/`
- **依赖**: [mongo-cxx-driver](https://mongodb.github.io/mongo-cxx-driver/) （vcpkg feature `database-mongodb`，含 `mongocxx` + `libmongoc` + `bsoncxx`）

## 构建启用

CMake 配置阶段打开 `SHIELD_BUILD_PLUGIN_MONGODB`：

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_MONGODB=ON
cmake --build build
```

该选项会触发：

1. `plugins/mongodb/` 下的 shared library 被构建，输出到 `plugins/database.mongodb/bin/`，文件名 `libshield_doc_mongodb.{dll,so,dylib}`。
2. vcpkg manifest feature `database-mongodb` 透传给 vcpkg，自动安装 `mongo-cxx-driver`（含 `mongocxx`、`libmongoc`、`bsoncxx` 的运行时库）。

`mongocxx::instance` 是进程级单例，由插件首次加载时初始化，进程退出前不释放。这是 mongocxx 官方要求，并非泄漏——多个 mongodb 实例之间共享同一个 instance。

## 配置 Schema

`manifest.yaml` 中声明的 `config_schema` 字段如下：

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `uri` | string（secret） | 是 | — | MongoDB 连接串，完整形式 `mongodb://[user:pass@]host1[:port1][,host2[:port2]...]/database?[options]`。包含 replica set、authSource、TLS、readPreference 等所有选项。 |
| `database` | string | 否 | URI 中的库名 | 显式覆盖逻辑库名。未填时使用 URI path 段的库；都缺省则回退到 `test`。 |
| `connect_timeout_ms` | integer | 否 | `5000` | 连接建立（含 pool acquire 等待）超时。范围 100-60000。 |
| `socket_timeout_ms` | integer | 否 | `30000` | 单次 socket 读/写超时。范围 100-300000。 |
| `pool_size` | integer | 否 | `4` | 每实例独享的 `mongocxx::pool` 大小。范围 1-256。 |

`uri` 是 secret 字段，host 在日志和 `shield.plugin.packages()` 返回值中脱敏。

### 完整 app.yaml 示例

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: db.player
      package: database.mongodb
      required: true
      config:
        uri: "mongodb://shield:secret@mongo-0:27017,mongo-1:27017,mongo-2:27017/game?replicaSet=rs0&readPreference=secondaryPreferred&maxPoolSize=64"
        database: game
        connect_timeout_ms: 5000
        socket_timeout_ms: 30000
        pool_size: 16
    - id: db.audit
      package: database.mongodb
      required: false
      config:
        uri: "${MONGO_AUDIT_URI}"
        pool_size: 4
  bindings:
    document.default: db.player
    document.audit: db.audit
```

两个实例完全独立：各自的 `mongocxx::pool`、各自的超时、各自的库选择。共享的只有进程级 `mongocxx::instance`（这是驱动设计）。

## 接口契约

本插件实现 [`include/shield/plugin/document.h`](https://github.com/cuihairu/shield/blob/main/include/shield/plugin/document.h) 定义的 `shield_document_v1` 结构体。所有方法都是 C 函数指针，由 host 通过 `instance->get_interface("shield.document.v1")` 取得。

### JSON over C ABI

document 接口的边界是字符串，不是 BSON：每个 filter / doc / update / pipeline / opts 都是 UTF-8 JSON 字符串。插件内部 `bsoncxx::json::parse` 解析为 BSON，MongoDB 扩展（`{"$oid": "..."}`、`{"$date": ...}`、`{"$gt": ...}` 等）原样透传给驱动。

这个设计带来三个性质：

- ABI 稳定：BSON 布局或驱动升级不会破坏 vtable，host 不需要 BSON 绑定。
- 跨语言友好：未来 Python / Lua host 不需要 bson 绑定也能调用。
- 类型零泄漏：`bsoncxx::document::view` 这类驱动特有类型不跨 DLL 边界。

### 连接生命周期

```c
struct shield_doc_conn* (*connect)(const struct shield_doc_connect_args* args,
                                   char* err_buf, int err_buf_size);
void (*disconnect)(struct shield_doc_conn* conn);
int  (*ping)(struct shield_doc_conn* conn);
```

| 方法 | 语义 |
|------|------|
| `connect` | 从实例 pool 中 acquire 一个 `mongocxx::client`，包装为 `shield_doc_conn*` 返回。pool 满时按 `connect_timeout_ms` 阻塞等待。失败返回 `NULL` 并写 `err_buf`。 |
| `disconnect` | 把 client 还回 pool。`NULL` 安全。 |
| `ping` | 执行 `ping` 命令探测活性，返回 1/0。 |

C ABI 路径每次调用都 acquire/release 一次；Lua 路径由插件自己管理 proxy 持有的 entry，减少 round-trip。

### CRUD

```c
int (*find)(conn, collection, filter_json, opts_json, cursor* out);
int (*find_one)(conn, collection, filter_json, opts_json, char** out_doc_json, result* out);
int (*insert_one)(conn, collection, doc_json, result* out);
int (*insert_many)(conn, collection, docs_json_array, result* out);
int (*update_one)(conn, collection, filter_json, update_json, opts_json, result* out);
int (*update_many)(conn, collection, filter_json, update_json, result* out);
int (*delete_one)(conn, collection, filter_json, result* out);
int (*delete_many)(conn, collection, filter_json, result* out);
int (*count)(conn, collection, filter_json, opts_json, int64_t* out_count);
```

约定：

- `filter_json` / `doc_json` / `update_json` 都是 JSON 字符串，`NULL` 视为 `{}`。
- `opts_json` 承载 `projection`、`sort`、`limit`、`skip`、`collation`、`upsert` 等驱动选项；`NULL` 表示无选项。
- 所有方法返回 0 表示 transport 成功（`result.success` 可能仍为 0 表示驱动级错误），非 0 表示连接 poison。
- `find` 返回 `cursor.docs_json`（JSON 数组字符串）；`find_one` 返回单文档字符串或 `NULL`。

### 聚合与索引

```c
int (*aggregate)(conn, collection, pipeline_json_array, opts_json, cursor* out);
int (*create_index)(conn, collection, keys_json, opts_json, result* out);
int (*drop_index)(conn, collection, index_name, result* out);
```

- `pipeline_json_array` 是完整的聚合管道 JSON 数组（`[{"$match": ...}, {"$group": ...}]`）。
- `keys_json` 形如 `{"field": 1}` 或 `{"a.b": -1}`；`opts_json` 可带 `name`、`unique`、`expireAfterSeconds` 等。

### 事务

```c
int (*begin)(struct shield_doc_conn* conn, struct shield_doc_result* out);
int (*commit)(struct shield_doc_conn* conn, struct shield_doc_result* out);
int (*rollback)(struct shield_doc_conn* conn, struct shield_doc_result* out);
```

底层走 `client_session` + `start_transaction` / `commit_transaction` / `abort_transaction`。要求 MongoDB 4.0+ 且 replica set（或 4.2+ 的分片集群）；单机 mongod 调用 `begin` 会驱动报错。

### cursor / result 内存所有权

```c
struct shield_doc_cursor {
    int success;
    const char* error_msg;
    const char* error_code;
    const char* docs_json;     // JSON array string; plugin-owned
    int64_t matched_count;
};

struct shield_doc_result {
    int success;
    const char* error_msg;
    const char* error_code;
    int64_t matched_count;
    int64_t modified_count;
    int64_t inserted_count;
    char* inserted_id_json;
    char* upserted_id_json;
};

void (*free_cursor)(struct shield_doc_cursor* cursor);
void (*free_result)(struct shield_doc_result* result);
```

`docs_json` / `inserted_id_json` / `upserted_id_json` / `error_msg` / `error_code` 都由插件 `malloc` 分配。host 用完后**必须**调用 `free_cursor` / `free_result`，由插件遍历释放内部所有字符串。`free_*` 不释放结构体本身。

## 使用示例

### C++ 侧（通过 binding 访问）

```cpp
#include "shield/plugin/document.h"
#include "shield/plugin/plugin_host.hpp"

auto* doc = shield::plugin::global_host()
              .get_by_binding<shield_document_v1>("document.default");
if (!doc) {
    return;  // binding 未配置或目标实例未启动
}

char err_buf[256] = {};
shield_doc_connect_args args{};
args.uri = "mongodb://localhost:27017";
args.database = "game";
args.connect_timeout_ms = 5000;
args.pool_size = 4;
shield_doc_conn* conn = doc->connect(&args, err_buf, sizeof(err_buf));
if (!conn) {
    shield::log::error(std::format("mongodb connect failed: {}", err_buf));
    return;
}

shield_doc_cursor cursor{};
const char* filter = R"({"player_id": "u-1001"})";
if (doc->find(conn, "inventory", filter, nullptr, &cursor) == 0 && cursor.success) {
    // cursor.docs_json 形如 [{"_id": {"$oid": "..."}, "items": [...]}, ...]
    auto docs = nlohmann::json::parse(cursor.docs_json);
    for (auto& d : docs) { /* 业务处理 */ }
}
doc->free_cursor(&cursor);
doc->disconnect(conn);
```

### Lua 侧

插件通过 `register_lua` 暴露 `shield.database.mongodb` callable namespace。业务代码按实例 ID 取得 proxy，proxy 持有一个 pool entry 直到 GC：

```lua
local mongo = shield.database.mongodb("document.default")

-- 插入一份文档
local ok, err = mongo:insert_one("inventory", {
    player_id = "u-1001",
    items = { { id = "potion", count = 5 } },
    updated_at = os.time(),
})

-- 查询单条
local ok, doc = mongo:find_one("inventory", { player_id = "u-1001" })

-- 聚合：分组统计
local ok, cur = mongo:aggregate("inventory", {
    { ["$match"] = { player_id = "u-1001" } },
    { ["$unwind"] = "$items" },
    { ["$group"] = { _id = "$items.id", total = { ["$sum"] = "$items.count" } } },
})

-- 事务（要求 replica set）
local ok, err = mongo:transaction(function(tx)
    mongo:update_one("wallet", { uid = "u-1001" }, { ["$inc"] = { gold = -100 } },
                     { session = tx })
    mongo:insert_one("ledger", { uid = "u-1001", delta = -100, reason = "buy" },
                     { session = tx })
end)
```

#### ObjectId helper

Lua 端附带 `oid` / `is_oid` / `oid_hex` 工具函数（由 `plugins/mongodb/lua/init.lua` 提供）：

```lua
local mongo = shield.database.mongodb("document.default")

-- 把 24 字符 hex 转 {"$oid": "..."} 形态
local filter = { _id = mongo.oid("507f1f77bcf86cd799439011") }

-- 从查询结果取回的 _id 提取 hex
local ok, doc = mongo:find_one("players", filter)
local hex = mongo.oid_hex(doc._id)  -- "507f1f77bcf86cd799439011"
```

具体 API 契约（参数顺序、错误返回形态）见 [Lua API](/lua-api)。

## 错误处理

`shield_doc_result.error_code` 是稳定字符串，由 `map_exception` 把 mongocxx / bsoncxx 异常映射为统一码：

| `error_code` | 触发条件 | 来源异常 |
|--------------|----------|----------|
| `connection_lost` | 连接未 acquire、socket 断开 | pool acquire / network failure |
| `connection_failed` | 连接建立或 pool 等待超时 | `mongocxx::connection_timeout_exception` |
| `db_query_failed` | 服务端操作失败（含 duplicate key、write error、operation_exception、bulk_write_exception）以及兜底未知异常 | `mongocxx::operation_exception`、`mongocxx::bulk_write_exception`、其他 |
| `mapper_unsafe_sql` | JSON / BSON 解析失败（filter 或 doc 不合法） | `bsoncxx::exception` |
| `plugin_error` | 事务状态非法（commit/rollback 无 active transaction）或 `mongocxx::logic_exception` | logic / 状态校验 |

业务侧应当区分：

- **可重试**：`connection_lost`、`connection_failed`。
- **数据约束错误**：duplicate key（在 `db_query_failed` 的 `error_msg` 中包含具体信息）。
- **不可重试**：`mapper_unsafe_sql`（filter 写错）、`plugin_error`（事务用错）。

## 部署

### 二进制位置

```
plugins/database.mongodb/
├── manifest.yaml
├── lua/
│   └── init.lua               # oid / is_oid / oid_hex helper
└── bin/
    ├── libshield_doc_mongodb.dll     # Windows
    ├── libshield_doc_mongodb.so      # Linux
    └── libshield_doc_mongodb.dylib   # macOS
```

### 运行时依赖

| 平台 | 依赖 |
|------|------|
| Windows | `mongocxx.dll`、`bsoncxx.dll`、`libmongoc-1.0.dll`、`libbson-1.0.dll`（vcpkg 安装到运行时路径） |
| Linux | `libmongocxx.so._abi`、`libbsoncxx.so._abi`、`libmongoc-1.0.so.0`、`libbson-1.0.so.0` |
| macOS | `libmongocxx._abi.dylib`、`libbsoncxx._abi.dylib`、`libmongoc-1.0.0.dylib`、`libbson-1.0.0.dylib` |

host 启动时 `dlopen` / `LoadLibrary` 加载插件 shared library；mongocxx 系列运行时库必须在动态链接器搜索路径上。vcpkg manifest mode 会自动复制运行时 DLL 到可执行目录。

### 集群拓扑要求

事务（`begin` / `commit` / `rollback`）依赖 MongoDB 4.0+ replica set 或 4.2+ sharded cluster。单机 mongod 不支持事务，调用会返回 `db_query_failed`。

生产部署建议：

- 至少 3 节点 replica set，避免单点故障。
- `readPreference=secondaryPreferred` 把读流量分到 secondary，主库只承担写。
- 在 URI 中配置 `maxPoolSize` 大于等于插件 `pool_size`，避免驱动层和应用层 pool 不一致。
- 启用 `retryWrites=true`（mongocxx 默认开），网络抖动时自动重试幂等写。

## 跨接口边界

`shield.document.v1` 和 `shield.database.v1` 是两套独立接口，**不互通**：

- `database.mongodb` 插件只实现 `shield.document.v1`，通过 `shield.database.mongodb("document.default")` 访问，不提供 SQL database 接口。
- 同一个 MongoDB 实例可以同时被 SQL 接口（如果将来有 `postgresql` 走 MongoDB Wire Protocol 之类的桥接，不在当前范围）和 document 接口访问；但 Shield 当前只通过本插件提供 document 接口。
- 业务侧同时需要 SQL 和 document 时，应当部署两个独立数据源（例如 MySQL 走结构化数据、MongoDB 走文档数据），分别配置 binding。

## 相关链接

- [插件系统](/plugin-system) — Shield 插件 v1 设计、ABI 契约、bootstrap pipeline
- [插件参考](/plugins/) — 官方插件清单（含其他数据库实现）
- [Shield 数据语义](/runtime-data) — 插件 namespace、binding 和数据访问架构
- [mongo-cxx-driver 文档](https://mongodb.github.io/mongo-cxx-driver/)
- [MongoDB 连接串语法](https://www.mongodb.com/docs/manual/reference/connection-string/)
- [MongoDB 聚合管道](https://www.mongodb.com/docs/manual/core/aggregation-pipeline/)
