# queue.redis

> 基于 Redis Streams 的消息队列插件，实现 `shield.queue.v1` 接口，支持 consumer group、ack 确认和持久化消费。

## 包信息

- **包 ID**: `queue.redis`
- **接口**: [`shield.queue.v1`](/plugin-system#interface-model)
- **Capabilities**: `streams`、`consumer-group`
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_PLUGIN_QUEUE_REDIS`
- **源码**: `plugins/queue.redis/`
- **依赖**: redis-plus-plus（redis++）、hiredis（通过 vcpkg）

## 构建启用

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_QUEUE_REDIS=ON
```

构建产物输出到 `<build>/plugins/queue.redis/`。

## 配置 Schema

配置通过 `plugins.instances[].config` 注入。

| 字段 | 类型 | 必填 | 默认值 | 说明 |
| --- | --- | --- | --- | --- |
| `host` | string | 是 | `127.0.0.1` | Redis 主机地址 |
| `port` | integer | 否 | `6379` | Redis 端口，范围 1-65535 |
| `password` | string | 否 | - | 鉴权密码，`secret: true` 在日志中脱敏 |
| `db` | integer | 否 | `0` | Redis DB 索引，范围 0-15 |
| `connect_timeout_ms` | integer | 否 | `5000` | 建连超时，范围 100-60000 毫秒 |
| `command_timeout_ms` | integer | 否 | `2000` | 命令超时，范围 100-60000 毫秒 |

完整 `app.yaml` 示例：

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

## 接口契约

源文件：`include/shield/plugin/queue.h`。v1 接口基于 Redis Streams（`XADD` / `XREADGROUP` / `XACK`），提供持久化、consumer group 竞争消费和 at-least-once 投递保证。

### 连接管理

```c
struct shield_queue_conn* (*connect)(const struct shield_queue_config* cfg,
                                     char* err_buf, int err_buf_size);
void (*disconnect)(struct shield_queue_conn* conn);
```

- `connect` — 建立 redis-plus-plus 连接池，返回 `shield_queue_conn*`。连接池同时服务于 `publish`（`XADD`）和 `subscribe`（`XREADGROUP`）。失败时 `err_buf` 写入异常信息，返回 `nullptr`。
- `disconnect` — 停止所有消费线程，join 后释放连接。

### 发布

```c
int (*publish)(struct shield_queue_conn* conn, const char* channel,
               const char* data, int data_len);
```

- 对应 Redis `XADD channel * data <payload>`。
- `channel` 作为 Redis Stream 的 key 名。
- `data` / `data_len` 作为 Stream entry 的字段值（单字段 `payload`）。
- 返回 `0` 表示成功，`-1` 表示失败。
- 线程安全：`publish` 走连接池，可与 `subscribe` 并发调用。

### 订阅

```c
int (*subscribe)(struct shield_queue_conn* conn, const char* channel,
                 shield_queue_on_message callback, void* user_data);
int (*unsubscribe)(struct shield_queue_conn* conn, const char* channel);
```

- `subscribe` — 创建（或加入）consumer group，spawn 消费线程执行 `XREADGROUP`。消息到达时调用 `callback`。同一 channel 重复订阅覆盖旧回调。
- `unsubscribe` — 停止该 channel 的消费线程，从 consumer group 退出。

### 消息回调签名

```c
typedef void (*shield_queue_on_message)(const char* channel,
                                        const char* data,
                                        int data_len,
                                        void* user_data);
```

- 在消费线程中同步调用。
- `data` / `data_len` 是 Stream entry 的 payload 字段。
- 回调返回后自动执行 `XACK`，确认消息已消费。回调抛异常时消息不 ack，下次可被重新消费（at-least-once）。
- `user_data` 透传 `subscribe` 时的指针。

## 使用示例

### C++（通过 binding）

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

`queue.redis` 通过 `register_lua` 暴露 callable table 形式的多实例 proxy：

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

可用方法包括 `publish`、`subscribe`、`unsubscribe`。`publish` 成功时返回 `true`；`subscribe` / `unsubscribe` 成功时返回 `true`。

## 特殊语义

### Redis Streams vs pub/sub

本插件使用 Redis Streams（`XADD` / `XREADGROUP` / `XACK`），不使用 pub/sub（`PUBLISH` / `SUBSCRIBE`）。

| 维度 | Streams（本插件） | pub/sub |
| --- | --- | --- |
| 持久化 | 是，Stream 数据保留直到显式删除 | 否，消息即发即丢 |
| 投递保证 | at-least-once（需 ack） | at-most-once |
| 多消费者 | consumer group 竞争消费 | 全部广播 |
| 历史 | 有，可回溯消费 | 无 |
| 离线补投 | 有，未 ack 的消息可被重新分配 | 无 |

### Consumer Group

每个 `subscribe(channel)` 调用会：

1. 创建（或确认已存在）名为 `shield` 的 consumer group（`XGROUP CREATE`）。
2. spawn 一个消费线程，以 consumer name `shield-<instance_id>-<channel>` 执行 `XREADGROUP GROUP shield <consumer> COUNT 10 BLOCK 2000 STREAMS channel >`。
3. 消息到达时调用回调，回调返回后执行 `XACK`。

多个实例订阅同一 channel 时，Redis 在 consumer group 内做竞争分配——每条消息只投递给一个 consumer。

### 投递保证

本插件提供 at-least-once：

- 消息通过 `XADD` 写入 Stream，持久化在 Redis 中。
- 消费者通过 `XREADGROUP` 拉取，处理后 `XACK`。
- 消费者崩溃或回调抛异常时，消息保持 pending 状态。
- 通过 `XCLAIM` / `XAUTOCLAIM`（当前未暴露，计划中）可将长时间 pending 的消息重新分配给其他 consumer。

### 消费线程模型

每个 `shield_queue_conn` 拥有：

- 1 个连接池（redis-plus-plus `Redis`）用于 `publish`（`XADD`）和 `subscribe`（`XREADGROUP` / `XACK`）
- 每个 subscribed channel 1 个消费线程

消费循环调用 `XREADGROUP ... BLOCK 2000`，超时后重新循环，异常时 sleep 100ms 重试。回调在该线程内同步派发。

### Stream Key 命名

`channel` 参数直接作为 Redis Stream 的 key 名。建议加业务前缀（如 `svc.matchmaking.player.login`），避免多服务共用一个 Redis 时撞名。

### 消息大小与速率

- 单条 Stream entry 建议控制在 KB 级。
- 高吞吐场景：`XADD` 天然有序，consumer group 内竞争消费，可水平扩展 consumer 数量。
- `MAXLEN` 选项（当前未暴露）可用于自动裁剪 Stream 长度，防止内存无限增长。

### 多实例隔离

不同 `shield_queue_conn*` 各自维护 consumer name。同一 channel 被多个 conn 订阅时，Redis consumer group 保证每条消息只投递给其中一个 conn。

## 错误处理

| 方法 | 返回值语义 |
| --- | --- |
| `connect` | `nullptr` 失败（`err_buf` 含异常信息），非空指针成功 |
| `publish` | `0` 成功，`-1` 失败 |
| `subscribe` | `0` 成功，`-1` 失败 |
| `unsubscribe` | `0` 成功，`-1` 失败 |
| `disconnect` | 无返回值，join 所有消费线程后释放资源 |

消费线程异常时自动重试。`disconnect` 等待所有线程退出后返回。

## 部署

### 二进制位置

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

### Redis 版本要求

Redis Streams 需要 **Redis 5.0+**（`XADD`、`XREADGROUP`、`XACK`、`XGROUP`）。Consumer group 的 `XAUTOCLAIM` 需要 Redis 6.2+。

### 连接数规划

每个 `shield_queue_conn` 使用 1 个连接池。每个 subscribed channel 有 1 个消费线程，消费线程通过连接池执行 `XREADGROUP`（带 BLOCK）。连接池大小应 >= subscribed channel 数。

### 高可用

Redis Cluster 模式下，Stream key 会根据 hash slot 分片。同一 Stream 的所有 consumer 必须连接到同一 shard。redis-plus-plus 的 cluster client 自动处理路由。Sentinel 模式下主从切换会导致 pending 消息需要重新分配。

### 数据持久化

Stream 数据持久化在 Redis 中，不受插件生命周期影响。`unsubscribe` / `disconnect` 不会删除 Stream 数据。业务侧需要显式 `DEL` 或配置 `MAXLEN` 来清理。

## 相关链接

- [插件系统](/plugin-system)
- [插件参考索引](/plugins/)
- [Redis Streams 文档](https://redis.io/docs/data-types/streams/)
- [redis-plus-plus](https://github.com/sewenew/redis-plus-plus)
