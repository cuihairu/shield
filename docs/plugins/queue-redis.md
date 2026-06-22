# queue.redis

> 基于 Redis pub/sub 的消息广播插件，实现 `shield.queue.v1` 接口。

## 包信息

- **包 ID**: `queue.redis`
- **接口**: [`shield.queue.v1`](/plugin-system#interface-model)
- **Capabilities**: `pubsub`
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

配置通过 `plugins.instances[].config` 注入，字段直接映射到 `shield_queue_config`。

| 字段 | 类型 | 必填 | 默认值 | 说明 |
| --- | --- | --- | --- | --- |
| `host` | string | 是 | `127.0.0.1` | Redis 主机地址 |
| `port` | integer | 否 | `6379` | Redis 端口，范围 1-65535 |
| `password` | string | 否 | - | 鉴权密码，`secret: true` 在日志中脱敏 |
| `db` | integer | 否 | `0` | Redis DB 索引，范围 0-15 |
| `connect_timeout_ms` | integer | 否 | `5000` | 建连超时，范围 100-60000 毫秒 |
| `command_timeout_ms` | integer | 否 | `1000` | 命令/读循环超时，范围 100-60000 毫秒 |

注意：queue.redis 默认 `command_timeout_ms` 为 `1000`（cache.redis 为 `5000`），因为订阅循环的 socket 超时直接决定消息消费延迟。把它设大可以减少空轮询，但会增加 `disconnect` 的响应延迟。

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
        command_timeout_ms: 1000
  bindings:
    queue.default: queue.events
```

## 接口契约

源文件：`include/shield/plugin/queue.h`。v1 接口故意收窄为 `publish` / `subscribe` / `unsubscribe` 三件事。旧 `shield_redis` 暴露过的 consumer group、ack、pull 队列等语义都不在 v1 范围（见 `docs/plugin-system.md` 的 queue 接口定义），相关遗留代码已被移除。

### 连接管理

```c
struct shield_queue_conn* (*connect)(const struct shield_queue_config* cfg,
                                     char* err_buf, int err_buf_size);
void (*disconnect)(struct shield_queue_conn* conn);
```

- `connect` — 建立一个 redis-plus-plus `Redis` 主连接（用于 `publish`）和一个 `Subscriber` 子连接（用于 `subscribe`）。建连成功后立即 spawn 一个后台消费线程，并返回 `shield_queue_conn*`。失败时 `err_buf` 写入异常信息，返回 `nullptr`。
- `disconnect` — 设置 `running = false`，关闭 subscriber（这是跨平台中断阻塞读取最干净的方式），join 消费线程，析构连接。

`connect` 之后无需 `start`，订阅循环已经在线程里跑起来，等待 `subscribe` 注册回调。

### 发布

```c
int (*publish)(struct shield_queue_conn* conn, const char* channel,
               const char* data, int data_len);
```

- 对应 Redis `PUBLISH channel data`。
- 返回值为该消息实际投递到的订阅者数量（Redis 服务端返回值）。`0` 表示当前没有订阅者，但消息已被服务端接受。
- `data_len <= 0` 时按 `strlen(data)` 处理；`data_len > 0` 时按二进制长度投递。
- 调用线程安全：`publish` 走主连接的连接池，可与 `subscribe` 同时进行。

### 订阅

```c
int (*subscribe)(struct shield_queue_conn* conn, const char* channel,
                 shield_queue_on_message callback, void* user_data);
int (*unsubscribe)(struct shield_queue_conn* conn, const char* channel);
```

- `subscribe` — 在内部 `subs` map 注册回调，并向 Redis 发送 `SUBSCRIBE channel`。返回后消息即开始流入回调。同一 channel 重复订阅会覆盖旧回调。
- `unsubscribe` — 发送 `UNSUBSCRIBE channel`，并从 map 移除回调。已派发到回调但尚未执行的消息不保证被取消。

### 消息回调签名

```c
typedef void (*shield_queue_on_message)(const char* channel,
                                        const char* data,
                                        int data_len,
                                        void* user_data);
```

- 在消费线程中同步调用。回调执行时间过长会阻塞该 channel 后续消息以及该 conn 的所有 channel 派发。
- `data` 指向 redis-plus-plus 内部缓冲，回调返回后即失效，需要保留请自行拷贝。
- `data_len` 始终显式传递，不要按 `\0` 截断。
- `user_data` 透传 `subscribe` 时的指针，常用于绑定业务 context。

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
cfg.connect_timeout_ms = 5000;
cfg.command_timeout_ms = 1000;

char err_buf[256];
shield_queue_conn* conn = queue->connect(&cfg, err_buf, sizeof(err_buf));
if (!conn) return;

struct Ctx { std::string tag; } ctx{"events"};

queue->subscribe(conn, "player.login",
    [](const char* ch, const char* data, int len, void* ud) {
        auto* c = static_cast<Ctx*>(ud);
        // 处理登录事件，按 len 读取 data
    }, &ctx);

// 别处：发布
queue->publish(conn, "player.login", R"({"uid":1234})", 13);

// 关闭
queue->unsubscribe(conn, "player.login");
queue->disconnect(conn);
```

### Lua（规划中）

v1 计划将 queue.redis 暴露到 `shield.queue.redis`：

```lua
local q = shield.queue.redis("queue.events")
q:subscribe("player.login", function(channel, data)
  -- 处理消息
end)
q:publish("player.login", json.encode({uid = 1234}))
```

当前实现尚未提供 Lua 绑定（`register_lua` 为空实现）。

## 特殊语义

### pub/sub vs list 队列

Redis pub/sub 是 fire-and-forget：消息只在有订阅者的瞬间被投递，没有持久化，也没有离线补投。

| 维度 | pub/sub（本插件） | list 队列（未实现） |
| --- | --- | --- |
| 持久化 | 否，发布即丢 | 是，BRPOP 取走前一直在 |
| 投递保证 | at-most-once | at-least-once（需 ack） |
| 多消费者 | 全部广播 | 竞争消费 |
| 历史 | 无 | 有（LPUSH 累积） |
| consumer group | 不支持 | Redis Streams 支持 |

如果业务需要“消息必达”或“消费失败重试”，不要用本插件。可以走 Redis Streams（v1 未封装）或外接 Kafka/RabbitMQ。

### 投递保证

本插件提供 at-most-once：消息最多被投递一次，可能零次（订阅者断线期间）。订阅者回调抛异常不会触发重投，消息直接丢失。回调内部失败必须自行落库或转交其他系统重试。

### consumer group

v1 不支持。旧 `shield_redis` 的 group/ack 代码已在重构中删除。计划中的替代方案是新增 `queue.stream` 插件封装 Redis Streams，但 v1 不承诺时间表。

### 订阅线程模型

每个 `shield_queue_conn` 拥有：

- 1 个主连接（redis-plus-plus `Redis`，含连接池）用于 `publish`
- 1 个订阅连接（`Subscriber`，单连接，无池）用于 `subscribe`
- 1 个后台消费线程

消费循环调用 `subscriber.consume()`，遇到 `TimeoutError` 正常继续，`ClosedError` 退出（`disconnect` 主动触发），其他异常 sleep 100ms 重试。回调在该线程内同步派发，多 channel 共享一个线程。

### 多实例隔离

不同 `shield_queue_conn*` 完全独立，各自维护连接和线程。同一 channel 被多个 conn 订阅时，Redis 会把消息广播给每个订阅连接，每个 conn 各自调用自己的回调。

## 错误处理

| 方法 | 返回值语义 |
| --- | --- |
| `connect` | `nullptr` 失败（`err_buf` 含异常信息），非空指针成功 |
| `publish` | 返回订阅者数量；`-1` 表示参数非法或异常 |
| `subscribe` | `0` 成功，`-1` 失败 |
| `unsubscribe` | `0` 成功，`-1` 失败 |
| `disconnect` | 无返回值，吞掉所有异常 |

Redis 订阅连接断开会抛 `ClosedError`，消费线程退出。当前实现不会自动重连，需要调用方发现后 `disconnect` + `connect` + 重新 `subscribe`。生产环境建议外层包一个 watchdog 定期 `PING` 或检查消费线程活性。

## 部署

### 二进制位置

```
<shield-runtime>/
└── plugins/
    └── queue.redis/
        ├── plugin.json
        └── bin/
            ├── libshield_queue_redis.dll      # Windows
            ├── libshield_queue_redis.so       # Linux
            └── libshield_queue_redis.dylib    # macOS
```

### Redis 版本要求

Redis pub/sub 自 2.0 即支持，本插件对版本要求极低。建议使用 5.0+ 以获得更稳定的连接处理。集群模式下 pub/sub 有特殊语义（sharded pubsub 需 7.0+），本插件当前按 standalone 假设使用。

### 连接数规划

每个 `shield_queue_conn` 占用 2 个 Redis 连接（1 主 + 1 订阅）。订阅连接是长连接，不会进连接池。规划 Redis `maxclients` 时需要按实例数 × 2 估算，并预留业务侧其他客户端。

### 高可用

Redis 主从切换会导致订阅连接断开。Sentinel / Cluster 模式下，redis-plus-plus 支持，但本插件未通过 schema 暴露 sentinel 配置。需要时可在 `extra_json` 扩展，或外接代理（如 HAProxy）做连接重定向。

### channel 命名

Redis pub/sub 的 channel 名按字面匹配，不做模式解析（除非使用 `PSUBSCRIBE`，本插件未暴露）。建议业务侧加业务前缀（如 `svc.matchmaking.player.login`），避免多服务共用一个 Redis 时撞名。

### 消息大小与速率

Redis 单条 pub/sub 消息上限约 512MB（受 Redis 协议最大 bulk size 限制），但实际不应超过 MB 级，否则会显著拖慢消费线程。高吞吐场景应考虑：

- 单 conn 订阅 channel 数控制在数十以内（每个 channel 都在消费线程串行派发）
- 单条消息控制在 KB 级，大负载改走缓存 key + 通知模式（`publish` 一个引用 ID，订阅方 `cache.get` 取数据）
- 跨进程广播使用 shard conn，避免单线程成为瓶颈

### 线程安全

| 入口 | 线程安全 |
| --- | --- |
| `connect` / `disconnect` | 同一 conn 不可并发调用 disconnect |
| `publish` | 线程安全（走连接池） |
| `subscribe` / `unsubscribe` | 线程安全（内部 `subs_mu` 保护 map） |
| 消费回调 | 单 conn 内串行，跨 conn 并行 |

业务侧应避免在回调里直接调用 `unsubscribe`（会修改正在迭代的 map，虽然实现用了锁但仍可能自死锁）。需要退出订阅时设置标志位，由别的线程触发 `unsubscribe`。

## 相关链接

- [插件系统](/plugin-system)
- [插件参考索引](/plugins/)
- [Redis Pub/Sub 文档](https://redis.io/docs/interact/pubsub/)
- [redis-plus-plus](https://github.com/sewenew/redis-plus-plus)
