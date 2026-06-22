# matchmaking.elo

> 基于 ELO 评分容差的贪心匹配引擎，实现 `shield.matchmaking.v1` 接口，支持单人（solo）与组队（party）入队，进程内队列无持久化。

## 包信息

- **包 ID**: `matchmaking.elo`
- **接口**: [`shield.matchmaking.v1`](/plugin-system#interface-model)
- **Capabilities**: `solo`, `party`
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_PLUGIN_MATCHMAKING`
- **源码**: `plugins/matchmaking_elo/`
- **依赖**: 仅 STL；无外部运行时依赖

## 构建启用

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_MATCHMAKING=ON
cmake --build build --config Release
```

产物部署到 `plugins/matchmaking.elo/bin/`。

## 配置 Schema

| 字段 | 类型 | 必填 | 默认值 | 范围 | 说明 |
| --- | --- | --- | --- | --- | --- |
| `max_players_per_match` | integer | 否 | 2 | 2–64 | 单场对局最大玩家数 |
| `match_timeout_seconds` | integer | 否 | 30 | 1–3600 | 玩家最长排队时间（当前仅作配置提示，匹配逻辑未实现 timeout） |
| `rating_tolerance` | number | 否 | 200.0 | 0–10000 | 评分容差，相邻玩家评分差不超过此值才能进同一场 |

`app.yaml` 示例：

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: match.main
      package: matchmaking.elo
      required: true
      config:
        max_players_per_match: 2
        match_timeout_seconds: 30
        rating_tolerance: 200.0
  bindings:
    matchmaking.default: match.main
```

## 接口契约

实现 `include/shield/plugin/matchmaking.h` 中的 `shield_matchmaking_v1` vtable。Instance 内维护一个 `unordered_map<player_id, PlayerEntry>` 作为队列，所有方法在同一个 `std::mutex` 下串行执行。

### connect / disconnect

```c
struct shield_matchmaking_conn* (*connect)(
    const struct shield_matchmaking_config* cfg,
    char* err_buf, int err_buf_size);
void (*disconnect)(struct shield_matchmaking_conn* conn);
```

`matchmaking.elo` 是无状态连接器（队列与配置都在 instance 上），`connect` 直接返回 `nullptr`，`disconnect` 为 no-op。Connection 句柄即 instance 本身。

### 玩家数据结构

```c
struct shield_matchmaking_player {
    const char* player_id;
    double rating;
    const char* party_id;       // NULL if solo
    const char* region;
    const char* metadata_json;
};
```

| 字段 | 说明 |
| --- | --- |
| `player_id` | 玩家唯一标识，作为队列主键 |
| `rating` | 当前 ELO 评分（例如 1200.0） |
| `party_id` | 组队标识；`NULL` 或空字符串表示单人 |
| `region` | 地区标签；当前实现不强制同区匹配，仅透传到结果 |
| `metadata_json` | 预留，透传到匹配结果 |

### enqueue

```c
int (*enqueue)(struct shield_matchmaking_conn* conn,
               const struct shield_matchmaking_player* player);
```

把玩家加入队列。如果 `player_id` 已存在，新条目覆盖旧的（评分/标签被替换）。返回 `0` 成功，`-1` 失败（参数为空或 `player_id` 为 `nullptr`）。

线程安全。同一玩家重复入队是幂等的覆盖语义，不会复制条目。

### dequeue

```c
int (*dequeue)(struct shield_matchmaking_conn* conn,
               const char* player_id);
```

从队列中移除指定玩家。返回 `0` 成功（即使玩家不在队列也返回成功语义由实现决定：当前实现下找不到返回 `-1`）。

### is_queued

```c
int (*is_queued)(struct shield_matchmaking_conn* conn,
                 const char* player_id);
```

返回 `1` 表示玩家在队列中，`0` 表示不在（或参数无效）。

### try_match

```c
int (*try_match)(struct shield_matchmaking_conn* conn,
                 struct shield_matchmaking_result* out);
```

核心匹配算法。返回 `0` 表示执行完成（不代表一定产生了对局，`out->match_count` 可能为 0）。

算法步骤：

1. 把队列中所有玩家按 `rating` 升序排序。
2. 贪心扫描：从最低分玩家开始，向后尝试把连续玩家归入同一场，条件是：
   - 当前组未满（`group.size() < max_players_per_match`）。
   - 候选玩家与当前组**最低分玩家**的评分差 `<= rating_tolerance`。
3. 凑齐 `max_players_per_match` 个玩家则成局，从队列移除；否则跳过当前起始玩家继续。
4. 所有成局的玩家写入 `out->matches` 数组，每场分配一个 `match_<id>`（递增整数）。
5. 没有形成任何对局时 `out->match_count = 0`、`out->matches = nullptr`。

结果结构：

```c
struct shield_matchmaking_match {
    const char* match_id;
    struct shield_matchmaking_player* players;
    int player_count;
    const char* server_address;
};

struct shield_matchmaking_result {
    int success;
    const char* error_code;
    const char* error_msg;
    struct shield_matchmaking_match* matches;
    int match_count;
};
```

- `match_id`：`match_1`、`match_2`、...，仅在本进程生命周期内唯一，重启后从 `match_1` 重新开始。
- `server_address`：当前实现始终为 `nullptr`，由调用方/上层分配游戏服务器后填充。
- 所有字符串与数组由 `free_result` 释放。

### update_rating

```c
int (*update_rating)(struct shield_matchmaking_conn* conn,
                     const char* player_id, double new_rating);
```

更新**仍在队列中**的玩家评分。如果玩家已被匹配出队或从未入队，返回 `-1`。对局结束后的最终评分更新应由业务层持久化（本插件不持有玩家主数据）。

### get_rating

```c
int (*get_rating)(struct shield_matchmaking_conn* conn,
                  const char* player_id, double* out_rating);
```

读取队列中某玩家的当前评分。返回 `0` 成功，`-1` 失败（不在队列）。不在队列时 `*out_rating` 被设为 0。

### queue_size

```c
int (*queue_size)(struct shield_matchmaking_conn* conn, int* out_size);
```

返回当前队列长度。返回 `0` 成功。

### free_result

```c
void (*free_result)(struct shield_matchmaking_result* result);
```

释放 `try_match` 返回结果中的所有 `matches` 数组及其内部的 `players`、`match_id`、`player_id`、`party_id`、`region`、`metadata_json`、`server_address`。调用方拿到 `try_match` 结果处理完后必须调用一次。

## 使用示例

### C++（通过 binding）

完整的入队 → 匹配 → 更新积分流程：

```cpp
#include "shield/plugin/matchmaking.h"

const shield_matchmaking_v1* mm = /* 从 matchmaking.default binding 拿到 */;
shield_matchmaking_conn* conn = /* 对应实例句柄 */;

// 1. 玩家入队
shield_matchmaking_player p1{};
p1.player_id = "alice";
p1.rating = 1200.0;
p1.party_id = nullptr;        // solo
p1.region = "us-east";
mm->enqueue(conn, &p1);

shield_matchmaking_player p2{};
p2.player_id = "bob";
p2.rating = 1180.0;
p2.region = "us-east";
mm->enqueue(conn, &p2);

shield_matchmaking_player p3{};
p3.player_id = "carol";
p3.rating = 1500.0;           // 评分差距过大，本轮匹配不到
mm->enqueue(conn, &p3);

// 2. 尝试匹配（通常由周期性 ticker 调用）
shield_matchmaking_result result{};
if (mm->try_match(conn, &result) == 0 && result.success) {
    for (int i = 0; i < result.match_count; ++i) {
        auto& m = result.matches[i];
        printf("match %s: %d players\n", m.match_id, m.player_count);
        for (int j = 0; j < m.player_count; ++j) {
            printf("  - %s (rating=%.1f)\n",
                   m.players[j].player_id, m.players[j].rating);
        }
    }
    mm->free_result(&result);
}

// 3. 排队期间玩家赢了若干局，评分上涨，更新队列中的评分
mm->update_rating(conn, "carol", 1450.0);

// 4. 玩家主动取消排队
mm->dequeue(conn, "carol");

// 5. 查询队列状态
int size = 0;
mm->queue_size(conn, &size);
```

业务层推荐流程：

```text
玩家点击"开始匹配" -> enqueue
后台 ticker 每 1 秒 -> try_match
有 match 产出 -> 分配游戏服 -> 通知客户端 -> 业务侧记录对局
对局结束 -> 业务侧持久化新评分（auth/matchmaking 不负责）
玩家取消 -> dequeue
```

### Lua（规划中）

`matchmaking.elo` 当前 `register_lua` 是空实现。计划中的 namespace 约定：

```lua
-- 未来将注册到 shield.matchmaking.elo
local mm = shield.matchmaking.elo("match.main")
mm:enqueue({ player_id = "alice", rating = 1200.0, region = "us-east" })
local result = mm:try_match()
for _, m in ipairs(result.matches) do
    -- 分配游戏服、通知客户端
end
mm:update_rating("carol", 1450.0)
mm:dequeue("carol")
```

## 平台特性

### Solo vs Party

当前实现的匹配粒度是单个 `player_id`：

- **Solo**：`party_id` 留空，按单人入队，`max_players_per_match` 决定一场人数。
- **Party**：调用方需要先把同队所有玩家用同一个 `party_id` 入队，然后由业务层确保 party 内玩家要么一起进同一场、要么都不进。**注意**：当前 v1 贪心算法按单个 `player_id` 评分排序归组，没有 party 原子性保证——`party_id` 字段仅作为透传标签，匹配逻辑未消费它。

完整的 party 原子匹配需要上层包装：

```cpp
// 业务侧保证：要么整队入队，要么整队出队；匹配结果检查同 party 的玩家是否都在
// 同一场，否则丢弃这次匹配（或回退为 party 内最强+最弱平均评分重新入队）。
```

### rating_tolerance 自适应

当前实现使用**固定** `rating_tolerance`。生产级匹配系统通常会随排队时间放宽容差（例如每 10 秒 +50）。这需要业务层实现：

```cpp
// 伪代码：后台 ticker
for (auto& [id, entry] : queue_snapshot) {
    auto wait = now - entry.enqueued_at;
    double effective_tolerance = cfg.rating_tolerance
                               + wait.count() * tolerance_growth_per_second;
    // 用 effective_tolerance 调用匹配...
}
```

由于当前 `try_match` 内部硬编码用 `cfg.rating_tolerance`，实现自适应需要扩展 matchmaking.h 接口（v1.1 候选）或在业务层做二次包装。

### 无持久化

队列完全在进程内存：

- 进程重启 → 队列清空，所有排队中的玩家需要客户端重新发起匹配请求。
- 多副本部署 → 每个副本有独立队列，玩家被路由到哪个副本就在哪个副本排队。横向扩展需要前置一致性哈希或匹配代理。

如需跨副本共享队列，应在业务层用 Redis ZSET（按评分排序）实现，把 `matchmaking.elo` 作为单副本内的算法引擎。

### match_id 语义

`match_<n>` 仅在本进程生命周期内递增唯一。重启后从 `match_1` 重新开始。业务层不应把 `match_id` 作为跨进程或跨重启的全局唯一 ID；如需全局唯一，应在业务侧用 UUID 重新生成。

## 错误处理

| 场景 | 返回值 | 说明 |
| --- | --- | --- |
| `enqueue` 参数为空 | `-1` | `conn` / `player` / `player_id` 为 `nullptr` |
| `dequeue` 玩家不在队列 | `-1` | 找不到匹配的 `player_id` |
| `is_queued` 玩家在队列 | `1` | 参数无效时返回 `0` |
| `try_match` 参数为空 | `-1` | `conn` / `out` 为 `nullptr` |
| `try_match` 内存分配失败 | `-1` | `out->match_count = 0` |
| `update_rating` 玩家不在队列 | `-1` | `*out_rating` 未定义 |
| `get_rating` 玩家不在队列 | `-1` | `*out_rating = 0` |
| `queue_size` 参数为空 | `-1` | `conn` / `out_size` 为 `nullptr` |

所有成功路径返回 `0`。错误不会通过 `shield_error_v1` 上报，仅通过返回值表达。

## 部署

### 二进制位置

```
plugins/matchmaking.elo/
├── manifest.yaml
└── bin/
    ├── libshield_matchmaking_elo.dll
    ├── libshield_matchmaking_elo.so
    └── libshield_matchmaking_elo.dylib
```

### 运行时依赖

无外部运行时依赖。仅用 STL（`std::unordered_map`、`std::vector`、`std::mutex`、`std::atomic`），编译时静态链接到 C++ 运行时即可。

### 单副本 vs 多副本

| 部署模式 | 推荐度 | 说明 |
| --- | --- | --- |
| 单副本 | 推荐 | 算法最简单，所有玩家共享一个队列 |
| 多副本 + 一致性哈希 | 可行 | 同一玩家总是路由到同一副本；评分更新需要业务层广播 |
| 多副本 + 共享 Redis 队列 | 推荐 | 业务层用 Redis ZSET 做全局队列，本插件退化为副本内算法工具 |

### 性能特性

- `try_match` 时间复杂度：O(N log N) 排序 + O(N) 扫描，其中 N 是当前队列长度。
- 1000 人队列下一次 `try_match` 通常在毫秒级。
- 所有操作在单一 mutex 下，高并发入队/出队可能成为瓶颈；建议后台单线程 ticker 周期性调用 `try_match`，业务线程只做 `enqueue`/`dequeue`。

### 与游戏服务器分配集成

`try_match` 返回的 `match.server_address` 当前为 `nullptr`，业务层应在拿到匹配结果后：

1. 调用游戏服分配服务（自研或 Kubernetes 自定义调度）。
2. 把分配到的地址通知客户端。
3. 持久化对局记录（用业务层的数据库，不是 matchmaking 插件）。

## 相关链接

- [插件系统](/plugin-system) — 接口模型、ABI 契约
- [插件参考索引](/plugins/) — 全部官方插件
- [ELO rating system](https://en.wikipedia.org/wiki/Elo_rating_system) — ELO 评分算法
- [TrueSkill](https://www.microsoft.com/en-us/research/project/trueskill-ranking-system/) — 微软团队评分系统，复杂场景参考
