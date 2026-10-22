# queue.redis

> A Redis Streams based message queue plugin implementing the `shield.queue.v1` interface, supporting consumer groups, ack confirmation, and durable consumption.

## Package Information

- **Package ID**: `queue.redis`
- **Interface**: `shield.queue.v1`
- **Capabilities**: `streams`, `consumer-group`
- **Version**: 1.0.0
- **CMake option**: `SHIELD_BUILD_PLUGIN_QUEUE_REDIS`
- **Source**: `plugins/queue.redis/`
- **Dependencies**: redis-plus-plus (redis++), hiredis (via vcpkg)

## Build Enable

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_QUEUE_REDIS=ON
```

Build artifacts output to `<build>/plugins/queue.redis/`.

## Config Schema

Configuration is injected via `plugins.instances[].config`.

| Field | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `host` | string | yes | `127.0.0.1` | Redis host address |
| `port` | integer | no | `6379` | Redis port, range 1-65535 |
| `password` | string | no | - | auth password; `secret: true` redacts it in logs |
| `db` | integer | no | `0` | Redis DB index, range 0-15 |
| `connect_timeout_ms` | integer | no | `5000` | connect timeout, range 100-60000 ms |
| `command_timeout_ms` | integer | no | `2000` | command timeout, range 100-60000 ms |

Complete `app.yaml` example:

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: queue.events
      package: queue.redis
      required: true
      config:
        host: "127.0.0.1"
        port: 6379
        db: 1
        connect_timeout_ms: 3000
        command_timeout_ms: 2000
  bindings:
    queue.default: queue.events
```

## Interface Contract

Source file: `include/shield/plugin/queue.h`. The v1 interface is based on Redis Streams (`XADD` / `XREADGROUP` / `XACK`), providing durability, consumer-group competing consumption, and at-least-once delivery guarantees.

### Connection Management

```c
struct shield_queue_conn* (*connect)(const struct shield_queue_config* cfg,
                                     char* err_buf, int err_buf_size);
void (*disconnect)(struct shield_queue_conn* conn);
```

- `connect` — establishes the connection source (Phase 2 dual path: when `dependencies: redis:` is wired,
  reuses `redis.driver`'s shared connection pool, see redis.driver; otherwise builds a
  redis-plus-plus pool from the instance's own config), returning `shield_queue_conn*`. The pool serves
  both `publish` (`XADD`) and `subscribe` (`XREADGROUP`). On failure, `err_buf` receives the error and
  `nullptr` is returned.
- `disconnect` — stops all consumer threads, joins them, then releases the connection.

### Publish

```c
int (*publish)(struct shield_queue_conn* conn, const char* channel,
               const char* data, int data_len);
```

- Corresponds to Redis `XADD channel * data <payload>`.
- `channel` is the Redis Stream key name.
- `data` / `data_len` are the Stream entry's field value (the single field `payload`).
- Returns `0` on success, `-1` on failure.
- Thread safety: `publish` goes through the pool and may run concurrently with `subscribe`.

### Subscribe

```c
int (*subscribe)(struct shield_queue_conn* conn, const char* channel,
                 shield_queue_on_message callback, void* user_data);
int (*unsubscribe)(struct shield_queue_conn* conn, const char* channel);
```

- `subscribe` — creates (or joins) a consumer group and spawns a consumer thread running `XREADGROUP`.
  Calls `callback` when messages arrive. Subscribing the same channel again replaces the old callback.
- `unsubscribe` — stops the channel's consumer thread and leaves the consumer group.

### Message Callback Signature

```c
typedef void (*shield_queue_on_message)(const char* channel,
                                        const char* data,
                                        int data_len,
                                        void* user_data);
```

- Called synchronously on the consumer thread.
- `data` / `data_len` are the Stream entry's payload field.
- `XACK` runs automatically after the callback returns, confirming consumption. If the callback throws,
  the message is not acked and can be re-consumed later (at-least-once).
- `user_data` passes through the pointer given to `subscribe`.

## Usage Examples

### C++ (via a binding)

```cpp
#include "shield/plugin/queue.h"
#include "shield/plugin/plugin_host.hpp"

auto queue = shield::plugin::global_host()
                 .get_by_binding<shield_queue_v1>("queue.default");

shield_queue_config cfg{};
cfg.host = "127.0.0.1";
cfg.port = 6379;
cfg.db = 1;

char err_buf[256];
shield_queue_conn* conn = queue->connect(&cfg, err_buf, sizeof(err_buf));
if (!conn) return;

struct Ctx { std::string tag; } ctx{"events"};

// 订阅：加入 consumer group，消费线程自动拉取
queue->subscribe(conn, "player.login",
    [](const char* ch, const char* data, int len, void* ud) {
        auto* c = static_cast<Ctx*>(ud);
        // 处理消息，按 len 读取 data
        // 回调返回后自动 XACK
    }, &ctx);

// 发布：XADD 到 stream
queue->publish(conn, "player.login", R"({"uid":1234})", 13);

// 取消订阅
queue->unsubscribe(conn, "player.login");
queue->disconnect(conn);
```

### Lua

`queue.redis` exposes a callable-table multi-instance proxy via `register_lua`:

```lua
local q = shield.queue.redis("queue.events")

local ok, err = q:subscribe("player.login", function(channel, data)
  -- 处理消息，返回后自动 XACK
end)
if not ok then
  -- 处理 err.message
end

local ok, err = q:publish("player.login", json.encode({uid = 1234}))

q:unsubscribe("player.login")
```

Available methods are `publish`, `subscribe`, `unsubscribe`. `publish` returns `true` on success;
`subscribe` / `unsubscribe` return `true` on success.

The Lua subscription delivery model (different from the C ABI):

- `subscribe` must be called inside a service handler or `on_init` (it needs an owner service); otherwise
  it returns `false, {code="invalid_context"}`.
- The consumer thread only runs `XREADGROUP` and **never touches Lua**. Each message is delivered to the
  owner service's actor via `host_api.lua_post_to_service`; the Lua callback and the subsequent `XACK` both
  execute at that actor's scheduling point, serialized with handlers.
- On service exit, undelivered messages are cancelled along with the actor's tasks and stay pending
  (at-least-once), available for other consumers to re-consume.

## Special Semantics

### Redis Streams vs pub/sub

This plugin uses Redis Streams (`XADD` / `XREADGROUP` / `XACK`), not pub/sub (`PUBLISH` / `SUBSCRIBE`).

| Dimension | Streams (this plugin) | pub/sub |
| --- | --- | --- |
| Durability | yes, Stream data persists until explicitly deleted | no, fire-and-forget |
| Delivery guarantee | at-least-once (ack required) | at-most-once |
| Multiple consumers | consumer-group competing consumption | broadcast to all |
| History | yes, consumable retroactively | none |
| Offline redelivery | yes, unacked messages can be reassigned | none |

### Consumer Group

Each `subscribe(channel)` call:

1. creates (or confirms the existence of) a consumer group named `shield` (`XGROUP CREATE`).
2. spawns a consumer thread that runs
   `XREADGROUP GROUP shield <consumer> COUNT 10 BLOCK 2000 STREAMS channel >` with consumer name
   `shield-<instance_id>-<channel>`.
3. calls the callback when messages arrive, then runs `XACK` after it returns.

When multiple instances subscribe to the same channel, Redis does competing assignment inside the consumer
group — each message is delivered to exactly one consumer.

### Delivery Guarantees

This plugin provides at-least-once:

- messages are written to the Stream via `XADD`, persisted in Redis.
- consumers pull with `XREADGROUP` and `XACK` after processing.
- when a consumer crashes or the callback throws, the message stays pending.
- `XCLAIM` / `XAUTOCLAIM` (not currently exposed, planned) can reassign long-pending messages to other
  consumers.

### Consumer Thread Model

Each `shield_queue_conn` owns:

- 1 connection pool (redis-plus-plus `Redis`) for `publish` (`XADD`) and `subscribe` (`XREADGROUP` / `XACK`)
- 1 consumer thread per subscribed channel

The consume loop calls `XREADGROUP ... BLOCK 2000`, re-loops on timeout, and on exception sleeps 100ms and
retries. Callbacks dispatch synchronously on that thread.

### Stream Key Naming

The `channel` argument is the Redis Stream key name directly. Prefer a business prefix (e.g.
`svc.matchmaking.player.login`) to avoid collisions when multiple services share one Redis.

### Message Size and Rate

- Keep individual Stream entries at the KB level.
- For high throughput: `XADD` is naturally ordered and consumer groups compete, so consumer count can scale
  horizontally.
- The `MAXLEN` option (not currently exposed) can auto-trim Stream length, preventing unbounded memory
  growth.

### Multi-Instance Isolation

Different `shield_queue_conn*`s each maintain their own consumer name. When one channel is subscribed by
multiple conns, the Redis consumer group guarantees each message goes to exactly one of them.

## Error Handling

| Method | Return semantics |
| --- | --- |
| `connect` | `nullptr` on failure (`err_buf` has the error), non-null on success |
| `publish` | `0` success, `-1` failure |
| `subscribe` | `0` success, `-1` failure |
| `unsubscribe` | `0` success, `-1` failure |
| `disconnect` | no return; joins all consumer threads then releases resources |

Consumer threads retry automatically on exception. `disconnect` returns after all threads exit.

## Deployment

### Binary Location

```
<shield-runtime>/
└── plugins/
    └── queue.redis/
        ├── manifest.yaml
        └── bin/
            ├── libshield_queue_redis.dll      # Windows
            ├── libshield_queue_redis.so       # Linux
            └── libshield_queue_redis.dylib    # macOS
```

### Redis Version Requirements

Redis Streams requires **Redis 5.0+** (`XADD`, `XREADGROUP`, `XACK`, `XGROUP`). The consumer group's
`XAUTOCLAIM` requires Redis 6.2+.

### Connection Planning

Each `shield_queue_conn` uses 1 connection pool. Each subscribed channel has 1 consumer thread, which runs
`XREADGROUP` (with BLOCK) through the pool. Pool size should be >= the subscribed channel count.

### High Availability

Under Redis Cluster, Stream keys shard by hash slot. All consumers of the same Stream must connect to the
same shard; redis-plus-plus's cluster client routes automatically. Under Sentinel, master failover means
pending messages need reassignment.

### Data Persistence

Stream data persists in Redis independent of the plugin lifecycle. `unsubscribe` / `disconnect` do not
delete Stream data. The business side must explicitly `DEL` or configure `MAXLEN` to clean up.

## Related Links

- Plugin System
- Plugin Reference Index
- [Redis Streams documentation](https://redis.io/docs/data-types/streams/)
- [redis-plus-plus](https://github.com/sewenew/redis-plus-plus)
