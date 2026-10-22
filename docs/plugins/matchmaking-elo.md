# matchmaking.elo

> A greedy matching engine with ELO-rating tolerance, implementing the `shield.matchmaking.v1` interface, supporting solo and party enqueue, with an in-process queue and no persistence.

## Package Information

- **Package ID**: `matchmaking.elo`
- **Interface**: `shield.matchmaking.v1`
- **Capabilities**: `solo`, `party`
- **Version**: 1.0.0
- **CMake option**: `SHIELD_BUILD_PLUGIN_MATCHMAKING`
- **Source**: `plugins/matchmaking_elo/`
- **Dependencies**: STL only; no external runtime dependencies

## Build Enable

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_MATCHMAKING=ON
cmake --build build --config Release
```

Artifacts deploy to `plugins/matchmaking.elo/bin/`.

## Config Schema

| Field | Type | Required | Default | Range | Description |
| --- | --- | --- | --- | --- | --- |
| `max_players_per_match` | integer | no | 2 | 2–64 | maximum players per match |
| `match_timeout_seconds` | integer | no | 30 | 1–3600 | maximum queue time (currently only a config hint; the matching logic implements no timeout) |
| `rating_tolerance` | number | no | 200.0 | 0–10000 | rating tolerance; adjacent players may only share a match when their rating gap is within this value |

`app.yaml` example:

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

## Interface Contract

Implements the `shield_matchmaking_v1` vtable in `include/shield/plugin/matchmaking.h`. The instance keeps an `unordered_map<player_id, PlayerEntry>` as the queue; all methods serialize under a single `std::mutex`.

### connect / disconnect

```c
struct shield_matchmaking_conn* (*connect)(
    const struct shield_matchmaking_config* cfg,
    char* err_buf, int err_buf_size);
void (*disconnect)(struct shield_matchmaking_conn* conn);
```

`matchmaking.elo` is a stateless connector (queue and config both live on the instance); `connect` returns
`nullptr` directly and `disconnect` is a no-op. The connection handle is the instance itself.

### Player Data Structure

```c
struct shield_matchmaking_player {
    const char* player_id;
    double rating;
    const char* party_id;       // NULL if solo
    const char* region;
    const char* metadata_json;
};
```

| Field | Description |
| --- | --- |
| `player_id` | the player's unique identifier; the queue's primary key |
| `rating` | the current ELO rating (e.g. 1200.0) |
| `party_id` | party identifier; `NULL` or empty string means solo |
| `region` | region label; the current implementation does not enforce same-region matching and only passes it through to results |
| `metadata_json` | reserved; passed through to match results |

### enqueue

```c
int (*enqueue)(struct shield_matchmaking_conn* conn,
               const struct shield_matchmaking_player* player);
```

Adds a player to the queue. If `player_id` already exists, the new entry overwrites the old (rating/labels
replaced). Returns `0` on success, `-1` on failure (null arguments or a
`nullptr` `player_id`).

Thread-safe. Re-enqueueing the same player is idempotent overwrite semantics; entries are never duplicated.

### dequeue

```c
int (*dequeue)(struct shield_matchmaking_conn* conn,
               const char* player_id);
```

Removes the given player from the queue. Returns `0` on success (even when the player is not queued,
returning success is implementation-defined: in the current implementation, not found returns `-1`).

### is_queued

```c
int (*is_queued)(struct shield_matchmaking_conn* conn,
                 const char* player_id);
```

Returns `1` if the player is queued, `0` if not (or the argument is invalid).

### try_match

```c
int (*try_match)(struct shield_matchmaking_conn* conn,
                 struct shield_matchmaking_result* out);
```

The core matching algorithm. Returns `0` meaning execution completed (not necessarily that a match was
produced; `out->match_count` may be 0).

Algorithm steps:

1. Sort all queued players by `rating` ascending.
2. Greedy scan: starting from the lowest-rated player, try to fold consecutive players into one match when:
   - the current group is not full (`group.size() < max_players_per_match`);
   - the candidate's rating gap to the group's **lowest-rated player** is `<= rating_tolerance`.
3. Once `max_players_per_match` players are gathered, the match forms and they leave the queue; otherwise
   skip the current starting player and continue.
4. All matched players are written into the `out->matches` array, each match assigned a `match_<id>`
   (an incrementing integer).
5. With no matches formed, `out->match_count = 0` and `out->matches = nullptr`.

Result structures:

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

- `match_id`: `match_1`, `match_2`, ..., unique only within this process's lifetime; after restart it starts
  again from `match_1`.
- `server_address`: always `nullptr` in the current implementation, to be filled by the caller/upper layer
  after assigning a game server.
- All strings and arrays are freed by `free_result`.

### update_rating

```c
int (*update_rating)(struct shield_matchmaking_conn* conn,
                     const char* player_id, double new_rating);
```

Updates the rating of a player **still in the queue**. If the player was already matched out or never
enqueued, returns `-1`. Post-match final rating updates should be persisted by the business layer (this
plugin holds no player master data).

### get_rating

```c
int (*get_rating)(struct shield_matchmaking_conn* conn,
                  const char* player_id, double* out_rating);
```

Reads a queued player's current rating. Returns `0` on success, `-1` on failure (not queued). When not
queued, `*out_rating` is set to 0.

### queue_size

```c
int (*queue_size)(struct shield_matchmaking_conn* conn, int* out_size);
```

Returns the current queue length. Returns `0` on success.

### free_result

```c
void (*free_result)(struct shield_matchmaking_result* result);
```

Frees all `matches` arrays in the `try_match` result and their inner `players`, `match_id`, `player_id`,
`party_id`, `region`, `metadata_json`, `server_address`. The caller must call this once after finishing with
the `try_match` result.

## Usage Examples

### C++ (via a binding)

The full enqueue → match → update flow:

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

Recommended business-layer flow:

```text
玩家点击"开始匹配" -> enqueue
后台 ticker 每 1 秒 -> try_match
有 match 产出 -> 分配游戏服 -> 通知客户端 -> 业务侧记录对局
对局结束 -> 业务侧持久化新评分（auth/matchmaking 不负责）
玩家取消 -> dequeue
```

### Lua (planned)

`matchmaking.elo`'s `register_lua` is currently an empty implementation. The planned namespace convention:

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

## Platform Characteristics

### Solo vs Party

The current implementation matches at the granularity of a single `player_id`:

- **Solo**: leave `party_id` empty and enqueue as an individual; `max_players_per_match` decides match size.
- **Party**: the caller must first enqueue all teammates with the same `party_id`, then the business layer
  must ensure the party either enters the same match together or not at all. **Note**: the current v1 greedy
  algorithm groups by individual `player_id` rating sort with no party atomicity guarantee — the `party_id`
  field is only a pass-through label and the matching logic does not consume it.

Full party-atomic matching needs an upper-layer wrapper:

```cpp
// 业务侧保证：要么整队入队，要么整队出队；匹配结果检查同 party 的玩家是否都在
// 同一场，否则丢弃这次匹配（或回退为 party 内最强+最弱平均评分重新入队）。
```

### Adaptive rating_tolerance

The current implementation uses a **fixed** `rating_tolerance`. Production matching systems usually widen the
tolerance with queue time (e.g. +50 every 10 seconds). The business layer implements this:

```cpp
// 伪代码：后台 ticker
for (auto& [id, entry] : queue_snapshot) {
    auto wait = now - entry.enqueued_at;
    double effective_tolerance = cfg.rating_tolerance
                               + wait.count() * tolerance_growth_per_second;
    // 用 effective_tolerance 调用匹配...
}
```

Since the current `try_match` hard-codes `cfg.rating_tolerance` internally, adaptive tolerance needs either a
matchmaking.h interface extension (a v1.1 candidate) or a second wrapper at the business layer.

### No persistence

The queue is entirely in process memory:

- process restart → queue cleared; all queued players must re-issue match requests from the client.
- multi-replica deployment → each replica has an independent queue; a player queues on whichever replica
  they were routed to. Horizontal scaling needs consistent hashing or a match proxy in front.

To share a queue across replicas, implement it in the business layer with a Redis ZSET (sorted by rating)
and use `matchmaking.elo` as the single-replica algorithm engine.

### match_id semantics

`match_<n>` increments uniquely only within this process's lifetime. After restart it starts again from
`match_1`. The business layer must not treat `match_id` as a globally unique ID across processes or restarts;
for global uniqueness, regenerate a UUID on the business side.

## Error Handling

| Scenario | Return | Description |
| --- | --- | --- |
| `enqueue` null argument | `-1` | `conn` / `player` / `player_id` is `nullptr` |
| `dequeue` player not queued | `-1` | no matching `player_id` found |
| `is_queued` player queued | `1` | invalid arguments return `0` |
| `try_match` null argument | `-1` | `conn` / `out` is `nullptr` |
| `try_match` allocation failure | `-1` | `out->match_count = 0` |
| `update_rating` player not queued | `-1` | `*out_rating` undefined |
| `get_rating` player not queued | `-1` | `*out_rating = 0` |
| `queue_size` null argument | `-1` | `conn` / `out_size` is `nullptr` |

All success paths return `0`. Errors are never reported through `shield_error_v1`; they are expressed only
via return values.

## Deployment

### Binary Location

```
plugins/matchmaking.elo/
├── manifest.yaml
└── bin/
    ├── libshield_matchmaking_elo.dll
    ├── libshield_matchmaking_elo.so
    └── libshield_matchmaking_elo.dylib
```

### Runtime Dependencies

No external runtime dependencies. STL only (`std::unordered_map`, `std::vector`, `std::mutex`,
`std::atomic`), statically linked to the C++ runtime at build time.

### Single Replica vs Multi-Replica

| Deployment | Recommendation | Notes |
| --- | --- | --- |
| single replica | recommended | the simplest algorithm; all players share one queue |
| multi-replica + consistent hashing | viable | the same player always routes to the same replica; rating updates need business-layer broadcast |
| multi-replica + shared Redis queue | recommended | the business layer builds the global queue on a Redis ZSET; this plugin degrades to a within-replica algorithm tool |

### Performance Characteristics

- `try_match` complexity: O(N log N) sort + O(N) scan, where N is the current queue length.
- With a 1000-player queue, one `try_match` is typically millisecond-level.
- All operations serialize on a single mutex; high-concurrency enqueue/dequeue may become the bottleneck.
  Prefer a background single-thread ticker calling `try_match` periodically, with business threads doing only
  `enqueue`/`dequeue`.

### Integration with Game Server Assignment

`try_match`'s returned `match.server_address` is currently `nullptr`; after obtaining match results the
business layer should:

1. call the game server assignment service (in-house or Kubernetes custom scheduling).
2. notify the client of the assigned address.
3. persist the match record (with the business layer's database, not the matchmaking plugin).

## Related Links

- Plugin System — interface model, ABI contract
- Plugin Reference Index — all official plugins
- [ELO rating system](https://en.wikipedia.org/wiki/Elo_rating_system) — the ELO algorithm
- [TrueSkill](https://www.microsoft.com/en-us/research/project/trueskill-ranking-system/) — Microsoft's team rating system, a reference for complex scenarios
