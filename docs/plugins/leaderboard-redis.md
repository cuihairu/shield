# leaderboard.redis

> 基于 Redis ZSET 的排行榜插件，实现 `shield.leaderboard.v1` 接口，支持多字段复合评分。

## 包信息

- **包 ID**: `leaderboard.redis`
- **接口**: [`shield.leaderboard.v1`](/plugin-system#interface-model)
- **Capabilities**: `sorted-set`、`multi-field`
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_PLUGIN_LEADERBOARD_REDIS`
- **源码**: `plugins/leaderboard.redis/`
- **依赖**: redis-plus-plus（redis++）、hiredis（通过 vcpkg）

## 构建启用

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_LEADERBOARD_REDIS=ON
```

构建产物输出到 `<build>/plugins/leaderboard.redis/`。

## 配置 Schema

连接层配置通过 `plugins.instances[].config` 注入，字段直接映射到 `shield_leaderboard_connect_args`。

| 字段 | 类型 | 必填 | 默认值 | 说明 |
| --- | --- | --- | --- | --- |
| `host` | string | 是 | `127.0.0.1` | Redis 主机地址 |
| `port` | integer | 否 | `6379` | Redis 端口，范围 1-65535 |
| `password` | string | 否 | - | 鉴权密码，`secret: true` 在日志中脱敏 |
| `db` | integer | 否 | `0` | Redis DB 索引，范围 0-15 |
| `connect_timeout_ms` | integer | 否 | `5000` | 建连超时，范围 100-60000 毫秒 |
| `command_timeout_ms` | integer | 否 | `5000` | 单条命令超时，范围 100-60000 毫秒 |

排行榜本身（board name、字段定义、排序方向）不通过 schema 配置，而是运行时通过 `create_board` API 注册。每个 board 对应 Redis 中的一个 ZSET key。

完整 `app.yaml` 示例：

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: leaderboard.global
      package: leaderboard.redis
      required: true
      config:
        host: "127.0.0.1"
        port: 6379
        db: 3
        connect_timeout_ms: 3000
        command_timeout_ms: 2000
  bindings:
    leaderboard.default: leaderboard.global
```

## 接口契约

源文件：`include/shield/plugin/leaderboard.h`。每个 board 是一个 Redis ZSET，key 为 board name，member 为 player_id，score 为按 board 配置编码出的复合值。

### 排序方向

```c
enum shield_sort_direction {
    SHIELD_SORT_DESC = 0,  // higher is better
    SHIELD_SORT_ASC  = 1,  // lower is better
};
```

每个字段可独立指定排序方向。`field_defs[0]`（主字段）的方向决定 `top_n` 是用 `ZREVRANGE`（DESC）还是 `ZRANGE`（ASC）。

### Board 配置

```c
struct shield_leaderboard_field_def {
    const char* name;
    enum shield_sort_direction dir;
};

struct shield_leaderboard_config {
    const char* name;  // board name
    struct shield_leaderboard_field_def* field_defs;
    int field_count;
    const char* backend;            // "memory"|"redis"|"sqlite"
    const char* backend_config_json;
    int max_entries;
    int auto_save_seconds;
};
```

- `name` — board 名，同时是 Redis ZSET 的 key。
- `field_defs` — 字段定义数组，字段顺序决定复合编码的优先级（前面字段先比较）。
- `backend` / `backend_config_json` — 当前实现忽略，固定走 Redis。
- `max_entries` / `auto_save_seconds` — 保留字段，当前实现未使用。

### 连接管理

```c
struct shield_leaderboard_conn* (*connect)(
    const struct shield_leaderboard_connect_args* args,
    char* err_buf, int err_buf_size);
void (*disconnect)(struct shield_leaderboard_conn* conn);
```

- `connect` — 建立 redis-plus-plus 连接，`PING` 校验，返回 conn。conn 内部维护 `boards` map 缓存每个 board 的字段定义和位宽。
- `disconnect` — 析构连接，丢弃 `boards` 缓存（board 本身在 Redis 里的 ZSET 数据不变）。

### Board 管理

```c
int (*create_board)(struct shield_leaderboard_conn* conn,
                    const struct shield_leaderboard_config* config,
                    char* err_buf, int err_buf_size);
int (*delete_board)(struct shield_leaderboard_conn* conn,
                    const char* board_name);
```

- `create_board` — 在 conn 内部 `boards` map 登记字段定义与位宽，不做 Redis 写操作。空字段定义会被替换为默认的 `{score, DESC}`。失败时 `err_buf` 写入异常信息。
- `delete_board` — Redis `DEL board_name`，并从 `boards` map 移除缓存。ZSET 不存在也返回 `0`。

### 条目操作

```c
int (*set_entry)(struct shield_leaderboard_conn* conn,
                 const char* board_name, const char* player_id,
                 const char* const* field_names, const double* field_values,
                 int field_count);
int (*remove_entry)(struct shield_leaderboard_conn* conn,
                    const char* board_name, const char* player_id);
int (*get_entry)(struct shield_leaderboard_conn* conn,
                 const char* board_name, const char* player_id,
                 struct shield_leaderboard_entry* out);
```

- `set_entry` — 把字段值编码成复合 score，Redis `ZADD board_name score player_id`。未调用 `create_board` 时按默认 `{score, DESC}` 编码。`field_names` 顺序无需匹配 board 定义顺序，内部按 name 查表重排；缺失字段按 0 处理。
- `remove_entry` — Redis `ZREM board_name player_id`。
- `get_entry` — Redis `ZSCORE` 拿到复合 score，按 board 字段定义解码回各字段值。player 不存在返回 `-1` 并清零 `out`。返回的 `field_names` / `field_values` 为 `malloc` 出的数组，调用方必须用 `free_entry` 释放。

### 排名查询

```c
int (*get_rank)(struct shield_leaderboard_conn* conn,
                const char* board_name, const char* player_id,
                int64_t* out_rank);
int (*top_n)(struct shield_leaderboard_conn* conn,
             const char* board_name, int n,
             struct shield_leaderboard_result* out);
```

- `get_rank` — Redis `ZRANK`（ASC 升序名次）+ 1，返回 1-based 排名。player 不存在时 `*out_rank = 0` 并返回 `-1`。**注意**：`ZRANK` 永远按 ASC 计算，DESC 排行榜需要业务自行换算 `total - rank + 1`。
- `top_n` — 主字段 DESC 时用 `ZREVRANGE`，ASC 时用 `ZRANGE`，取前 n 名。返回 `shield_leaderboard_result`，包含 `entries` 数组（仅 player_id 和 rank，不含字段值）。`n <= 0` 返回 `-1` 并置 `success = 0`。

### 内存管理

```c
void (*free_result)(struct shield_leaderboard_result* result);
void (*free_entry)(struct shield_leaderboard_entry* entry);
```

`get_entry` 返回的 entry 和 `top_n` 返回的 result 都包含 `malloc` 出的字符串和数组，必须用对应的 free 函数释放。重复 free 安全（内部置空）。

## 使用示例

### C++（通过 binding）

```cpp
#include "shield/plugin/leaderboard.h"
#include "shield/plugin/plugin_host.hpp"

auto lb = shield::plugin::global_host()
              .get_by_binding<shield_leaderboard_v1>("leaderboard.default");

shield_leaderboard_connect_args args{};
args.host = "127.0.0.1";
args.port = 6379;
args.db = 3;

char err_buf[256];
shield_leaderboard_conn* conn = lb->connect(&args, err_buf, sizeof(err_buf));
if (!conn) return;

// 定义 board：score 主排序（DESC），level 次排序（DESC）
shield_leaderboard_field_def defs[] = {
    {"score", SHIELD_SORT_DESC},
    {"level", SHIELD_SORT_DESC},
};
shield_leaderboard_config cfg{};
cfg.name = "lb:global";
cfg.field_defs = defs;
cfg.field_count = 2;
lb->create_board(conn, &cfg, err_buf, sizeof(err_buf));

// 写入条目
const char* names[] = {"score", "level"};
const double vals[] = {12500.0, 47.0};
lb->set_entry(conn, "lb:global", "player:1", names, vals, 2);

// 查排名
int64_t rank = 0;
lb->get_rank(conn, "lb:global", "player:1", &rank);

// 取前 10
shield_leaderboard_result res{};
lb->top_n(conn, "lb:global", 10, &res);
for (int i = 0; i < res.entry_count; ++i) {
    // res.entries[i].player_id, res.entries[i].rank
}
lb->free_result(&res);

lb->disconnect(conn);
```

### Lua

`leaderboard.redis` 通过 `register_lua` 暴露 callable table 形式的多实例 proxy：

```lua
local lb = shield.leaderboard.redis("leaderboard.default")

lb:create_board("lb:global", {
  fields = {
    { name = "score", order = "desc" },
    { name = "level", order = "desc" },
  },
})

local ok, err = lb:set_entry("lb:global", "player:1", {
  score = 12500,
  level = 47,
})
if not ok then
  -- 处理 err.message
end

local ok, rank_or_err = lb:get_rank("lb:global", "player:1")
local ok, top_or_err = lb:top_n("lb:global", 10)
```

可用方法包括 `create_board`、`delete_board`、`set_entry`、`remove_entry`、`get_entry`、`get_rank`、`top_n`。Lua 方法通常返回 `ok, result_or_error`；`get_entry` 未命中时返回 `true, nil`，`get_rank` 未上榜时返回 `true, 0`。

## 特殊语义

### ZSET + 复合评分编码

Redis ZSET 的 score 是单个 double，无法直接表达多字段排序。本插件沿用旧 `shield_redis.cpp` 的复合编码方案：

1. 每个 field 占固定 bit 宽度（默认 16 bit，最大合计 64 bit）。
2. 各字段按 board 配置顺序，MSB-first 拼接成一个 `uint64_t`。
3. 该 `uint64_t` 转成 double 作为 ZSET score。

字段顺序就是优先级：前面的字段 dominate 后面的。例如 `{score, level}` 编码后，score 高的无论 level 多少都排在前面；score 相同时 level 高的排前面。

编码逻辑（`encode_composite`）：

```
composite = 0
shift = total_bits
for each field i:
    shift -= field_bits[i]
    composite |= (uint64(value[i]) & ((1 << field_bits[i]) - 1)) << shift
```

解码（`decode_composite`）反向移位掩码。

### 位宽与取值范围

默认每个字段 16 bit，可表达 `[0, 65535]`。超出范围会被截断到该位宽（`value & mask`），不会进位到上一字段。生产环境如果需要更大范围，应通过 board 配置调整位宽（当前 schema 未通过 config 暴露，需修改插件代码或扩展 `extra_json`）。

总位宽硬上限 64 bit（`kMaxFieldBits`）。超过会被截断，超出部分的字段不参与编码。

### 字段顺序无关

`set_entry` 的 `field_names` 数组顺序无需匹配 `create_board` 时的定义顺序。内部按 name 查表重排到 board 定义顺序再编码。缺失字段按 0 处理，重复字段以后者为准。

### 同分排名规则

ZSET 内部对相同 score 的 member 按字典序（member 字符串）升序排列。因此：

- 复合编码完全相同（所有字段值都相同）的两个 player，按 player_id 字典序排在相邻名次。
- `ZRANK` / `ZREVRANGE` 返回的名次是严格的 ZSET 排序，不会因同分跳号。

### 主字段方向驱动 top_n

`top_n` 是否返回“从高到低”取决于 board 配置中 `field_defs[0].dir`：

| 主字段方向 | top_n 使用的 Redis 命令 | 语义 |
| --- | --- | --- |
| `SHIELD_SORT_DESC`（默认） | `ZREVRANGE` | 第一名是最高分 |
| `SHIELD_SORT_ASC` | `ZRANGE` | 第一名是最低分（如高尔夫、用时排名） |

后续字段的 `dir` 当前只参与编码时的取值，不影响 `top_n` 的方向选择（因为只有主字段决定整体升/降序）。多字段混合方向（如 score DESC + penalty ASC）当前编码不支持反序，业务需要把 penalty 转换为 `max - penalty` 再写入。

### get_rank 的方向陷阱

`get_rank` 直接调用 Redis `ZRANK`，永远返回 ASC 升序名次（0-based + 1）。对 DESC 排行榜，业务看到的“第 1 名”会拿到 ZSET 内的最大 rank 值，与直觉相反。换算公式：

```
real_rank_desc = total_entries - zrank_asc + 1
```

`total_entries` 需要额外调用 `ZCARD`（v1 接口未暴露），或在业务层维护。

## 错误处理

### 返回码

| 方法 | 成功 | 失败 |
| --- | --- | --- |
| `connect` | 非 null | `nullptr`，`err_buf` 写入异常 |
| `create_board` | `0` | `-1`，`err_buf` 写入异常 |
| `delete_board` | `0` | `-1` |
| `set_entry` | `0` | `-1` |
| `remove_entry` | `0` | `-1` |
| `get_entry` | `0` | `-1`（player 不存在或异常） |
| `get_rank` | `0` | `-1`，`*out_rank = 0` |
| `top_n` | `0` | `-1`，`out->success = 0`，`error_code` 填充 |

### shield_leaderboard_result 字段

```c
struct shield_leaderboard_result {
    int success;
    const char* error_code;
    const char* error_msg;
    struct shield_leaderboard_entry* entries;
    int entry_count;
};
```

- `success` — `1` 成功，`0` 失败。
- `error_code` — 失败时的稳定错误码：`invalid_args`（参数非法）、`lb_query_failed`（Redis 异常，`error_msg` 含异常 `what()`）。
- `error_msg` — 失败时由 `malloc` 分配，`free_result` 会一起释放。
- `entries` / `entry_count` — 成功时的结果数组，失败时为 `nullptr` / `0`。

调用方应优先检查 `success`，再访问 `entries`。

## 部署

### 二进制位置

```
<shield-runtime>/
└── plugins/
    └── leaderboard.redis/
        ├── plugin.json
        └── bin/
            ├── libshield_leaderboard_redis.dll      # Windows
            ├── libshield_leaderboard_redis.so       # Linux
            └── libshield_leaderboard_redis.dylib    # macOS
```

### Redis 版本要求

依赖 Redis ZSET 全套命令（`ZADD`/`ZSCORE`/`ZRANK`/`ZRANGE`/`ZREVRANGE`/`ZREM`/`DEL`），Redis 2.0+ 即可。建议 5.0+。

### 数据规模与内存

每个 player 占用 ZSET 中约 80-120 字节（member 字符串 + score）。百万级排行榜约占 100MB。ZSET 在 member 数 < 128 时使用 ziplist 紧凑编码，超过后切换 skiplist，建议根据规模评估内存预算。

### Board 命名

board name 直接作为 Redis key，建议加前缀避免与业务其他 key 冲突（如 `lb:global`、`lb:season:2026q2`）。不同 conn 的 `boards` map 是独立的，重复 `create_board` 同名 board 不影响 Redis 里的数据，只刷新本地字段定义缓存。

### 连接数

每个 `shield_leaderboard_conn` 使用一个 redis-plus-plus `Redis` 对象（无连接池配置，单连接）。并发查询会串行化在该连接上。高 QPS 场景建议拆分多个实例，或修改插件源码启用连接池（参考 cache.redis 的实现）。

## 相关链接

- [插件系统](/plugin-system)
- [插件参考索引](/plugins/)
- [Redis Sorted Set 文档](https://redis.io/docs/data-types/sorted-sets/)
- [redis-plus-plus](https://github.com/sewenew/redis-plus-plus)
