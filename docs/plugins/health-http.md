# health.http

> HTTP 健康检查与就绪探针端点，实现 `shield.health.v1` 接口，内嵌 boost::beast HTTP 服务端，聚合所有注册的检查项，输出 JSON，兼容 Kubernetes liveness/readiness probe。

## 包信息

- **包 ID**: `health.http`
- **接口**: [`shield.health.v1`](/plugin-system#interface-model)
- **Capabilities**: `http`, `k8s_probes`
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_PLUGIN_HEALTH`
- **源码**: `plugins/health_http/`
- **依赖**: boost::beast、boost::asio；不依赖 OpenSSL、不依赖 shield_net

## 构建启用

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_HEALTH=ON
cmake --build build --config Release
```

产物部署到 `plugins/health.http/bin/`。

## 配置 Schema

| 字段 | 类型 | 必填 | 默认值 | 范围 | 说明 |
| --- | --- | --- | --- | --- | --- |
| `bind_address` | string | 否 | `0.0.0.0` | — | HTTP 监听地址 |
| `port` | integer | 否 | 8086 | 1–65535 | HTTP 监听端口 |
| `liveness_path` | string | 否 | `/health` | — | liveness probe 路径 |
| `readiness_path` | string | 否 | `/ready` | — | readiness probe 路径 |

`app.yaml` 示例：

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: health.main
      package: health.http
      required: true
      config:
        bind_address: "0.0.0.0"
        port: 8086
        liveness_path: "/health"
        readiness_path: "/ready"
  bindings:
    health.default: health.main
```

## 接口契约

实现 `include/shield/plugin/health.h` 中的 `shield_health_v1` vtable。Instance 维护一个检查项列表（`std::vector<unique_ptr<CheckEntry>>`），由业务模块通过 `register_check` 注册。

### 状态枚举

```c
enum shield_health_status {
    SHIELD_HEALTH_OK       = 0,
    SHIELD_HEALTH_DEGRADED = 1,
    SHIELD_HEALTH_FAIL     = 2,
};
```

聚合规则：任一 check 返回 FAIL → 聚合状态为 FAIL；任一 check 返回 DEGRADED 且无 FAIL → 聚合为 DEGRADED；全部 OK → 聚合为 OK。

### connect / disconnect

```c
struct shield_health_session* (*connect)(const struct shield_health_config* cfg,
                                         char* err_buf, int err_buf_size);
void (*disconnect)(struct shield_health_session* session);
```

`connect` 是 no-op（HTTP 监听器在 `instance->start()` 阶段启动），`disconnect` 也是 no-op（监听器在 `shutdown()` 关闭）。Session 句柄即 instance 本身。

### register_check

```c
int (*register_check)(struct shield_health_session* session,
                      const char* name,
                      int (*check)(struct shield_health_check_result* out,
                                   void* user_data),
                      void* user_data);
```

注册一个命名检查项。`name` 是检查项标识（出现在 JSON 输出中），`check` 是回调函数，`user_data` 透传给回调。线程安全（内部加锁 push_back 到 vector）。

回调签名：

```c
int (*check)(shield_health_check_result* out, void* user_data);
```

- 返回 `0` 表示检查正常执行（最终状态看 `out->status`）。
- 返回非 `0` 视为检查失败，强制 `out->status = SHIELD_HEALTH_FAIL`。
- 回调应填充 `out->status`、`out->message`（可选），`out->check_name` 与 `out->latency_ms` 由插件填充。
- 回调内分配的 `out->message` 由插件 `free_result` 释放。

### check_all

```c
int (*check_all)(struct shield_health_session* session,
                 struct shield_health_check_result* results,
                 int max_results, int* out_count);
```

主动触发所有注册的检查项，把结果写入调用方提供的 `results` 数组（容量 `max_results`）。`*out_count` 写入实际填充数量（不会超过 `max_results`）。返回 `0` 成功。

适合内部周期性自检、不依赖 HTTP 探针的场景。

### get_status

```c
enum shield_health_status (*get_status)(struct shield_health_session* session);
```

返回最近一次 HTTP 请求或 `check_all` 计算出的聚合状态。初始值为 `SHIELD_HEALTH_OK`。每次 HTTP 探针请求都会更新这个值。

### free_result

```c
void (*free_result)(struct shield_health_check_result* result);
```

释放单个 `shield_health_check_result` 内的 `check_name` 与 `message` 字段（内部用 `std::malloc` 分配）。

## HTTP 端点格式

监听 `bind_address:port`，对每个连接同步读取请求、写入响应。仅响应 `GET`。

### 路径

| 路径 | 默认值 | 语义 |
| --- | --- | --- |
| `liveness_path` | `/health` | 进程存活探针：只要 HTTP 服务能响应就返回 200 |
| `readiness_path` | `/ready` | 就绪探针：聚合所有检查项，全部 OK/DEGRADED 才返回 200 |

当前实现下，两个路径走**相同的聚合逻辑**（都跑全部注册的检查项），区别仅在 Kubernetes 语义层面：

- liveness probe 失败 → kubelet 重启 Pod
- readiness probe 失败 → 从 Service endpoints 摘除流量

如需 liveness 永远返回 200（只确认进程没死锁），可以在 manifest 中只把真正关键的检查注册到 readiness 路径，或后续为 liveness 单独维护一个 lightweight check 子集。

### 响应

成功（聚合 OK 或 DEGRADED）：HTTP 200。

失败（聚合 FAIL）：HTTP 503（`http::status::service_unavailable`）。

未命中路径：HTTP 404。

Content-Type: `application/json`。

响应体示例：

```json
{
  "status": "ok",
  "checks": [
    {
      "name": "database",
      "status": "ok",
      "latency_ms": 3,
      "message": ""
    },
    {
      "name": "redis",
      "status": "degraded",
      "latency_ms": 152,
      "message": "latency above threshold"
    }
  ]
}
```

字段说明：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `status` | string | `ok` / `degraded` / `fail`，聚合值 |
| `checks[].name` | string | 来自 `register_check` 的 name |
| `checks[].status` | string | 单项状态 |
| `checks[].latency_ms` | integer | 回调执行耗时，由插件测量 |
| `checks[].message` | string | 仅当回调填了 `out->message` 才输出 |

注意：响应体由插件用 `std::ostringstream` 拼接，键名 `"checks"` 在源码中是固定字符串。如需修改输出结构，需要改 `handle_session` 内的拼接逻辑。

## 使用示例

### C++（通过 binding）

注册数据库健康检查 + 自定义检查项：

```cpp
#include "shield/plugin/health.h"

const shield_health_v1* health = /* 从 health.default binding 拿到 */;
shield_health_session* session = /* 对应实例句柄 */;

// 1. 注册数据库 ping 检查
struct DbCtx { /* 数据库连接指针 */ };
static DbCtx db_ctx{ /* ... */ };

auto db_check = [](shield_health_check_result* out, void* user_data) -> int {
    auto* ctx = static_cast<DbCtx*>(user_data);
    auto t0 = std::chrono::steady_clock::now();
    bool ok = ctx->ping();  // 业务侧实现的 ping
    auto t1 = std::chrono::steady_clock::now();
    out->latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          t1 - t0).count();
    if (!ok) {
        out->status = SHIELD_HEALTH_FAIL;
        out->message = strdup("database unreachable");
        return -1;
    }
    out->status = out->latency_ms > 100 ? SHIELD_HEALTH_DEGRADED : SHIELD_HEALTH_OK;
    return 0;
};
health->register_check(session, "database", db_check, &db_ctx);

// 2. 注册 Redis 检查（类似）
struct RedisCtx { /* ... */ };
static RedisCtx redis_ctx{ /* ... */ };
auto redis_check = [](shield_health_check_result* out, void* user_data) -> int {
    auto* ctx = static_cast<RedisCtx*>(user_data);
    if (!ctx->ping()) {
        out->status = SHIELD_HEALTH_FAIL;
        out->message = strdup("redis unreachable");
        return -1;
    }
    out->status = SHIELD_HEALTH_OK;
    return 0;
};
health->register_check(session, "redis", redis_check, &redis_ctx);

// 3. 主动触发一次完整自检（不依赖 HTTP）
shield_health_check_result results[8]{};
int count = 0;
health->check_all(session, results, 8, &count);
for (int i = 0; i < count; ++i) {
    // 处理 results[i]...
    health->free_result(&results[i]);
}

// 4. 读取最近一次聚合状态
auto status = health->get_status(session);
```

### Lua（规划中）

`health.http` 当前 `register_lua` 是空实现。计划中的 namespace 约定：

```lua
-- 未来将注册到 shield.health.http
local h = shield.health.http("health.main")
h:register("database", function()
    return "ok", "db reachable"
end)
local status = h:status()  -- "ok" / "degraded" / "fail"
```

## 平台特性

### 默认端口

8086。与 `metrics.prometheus`（8087）紧邻，便于运维统一管理平台服务端口。`SO_REUSEADDR` 已开启，进程重启可快速绑回。

### Liveness vs Readiness 语义

| 探针类型 | 路径 | 失败后果 | 推荐检查内容 |
| --- | --- | --- | --- |
| liveness | `/health` | kubelet 重启 Pod | 仅最关键的死锁检测；数据库短暂抖动不应触发重启 |
| readiness | `/ready` | 从 Service endpoints 摘除 | 所有下游依赖（数据库、缓存、消息队列）连通性 |

当前 v1 实现中两个路径共享同一组检查项。生产环境推荐做法：

- 把数据库/缓存 ping 注册为 readiness 检查。
- liveness 仅依赖进程响应 HTTP 这一事实本身（不注册任何检查），失败才意味着进程死锁。

### HTTP 服务端实现

内嵌 boost::beast 同步 accept 循环，单线程串行处理。健康检查 QPS 通常 < 10（kubelet 默认每 10 秒探一次），单线程绰绰有余。每个检查回调的执行时间直接累加到响应延迟，因此回调应避免长时间阻塞操作。

### Kubernetes 探针配置

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: shield
spec:
  template:
    spec:
      containers:
        - name: shield
          ports:
            - containerPort: 8086
              name: health
          livenessProbe:
            httpGet:
              path: /health
              port: 8086
            initialDelaySeconds: 30
            periodSeconds: 10
            timeoutSeconds: 5
            failureThreshold: 3
          readinessProbe:
            httpGet:
              path: /ready
              port: 8086
            initialDelaySeconds: 5
            periodSeconds: 5
            timeoutSeconds: 3
            failureThreshold: 2
```

- `initialDelaySeconds` 要长于应用启动时间（数据库迁移、缓存预热等）。
- `timeoutSeconds` 应大于所有检查回调的最大耗时之和。
- `failureThreshold` 控制容忍连续失败次数，避免单次抖动触发重启。

### Startup probe（Kubernetes 1.16+）

慢启动应用建议加 `startupProbe`，让 liveness/readiness 在启动完成前不生效：

```yaml
startupProbe:
  httpGet:
    path: /ready
    port: 8086
  failureThreshold: 30
  periodSeconds: 10
```

## 错误处理

| 场景 | 返回值 | 说明 |
| --- | --- | --- |
| `register_check` 参数为空 | `-1` | `session` / `name` / `check` 为 `nullptr` |
| `register_check` 正常 | `0` | 检查项加入列表 |
| `check_all` 参数为空 | `-1` | `session` / `results` / `out_count` 为 `nullptr` |
| 检查回调返回非 0 | 强制 FAIL | 即使 `out->status` 是 OK 也覆盖为 FAIL |
| HTTP 端口绑定失败（`start` 阶段） | `1` 并填 `plugin.init.failed` | 端口被占用或权限不足 |

HTTP 端的错误语义：

- 聚合 FAIL → 503，但响应体仍然完整（包含所有 check 的状态），便于排障。
- 单个 check 回调抛异常不会让整个 HTTP 服务崩溃（`handle_session` 内 try/catch 包裹读写，但回调本身的异常需要回调自己处理）。

## 部署

### 二进制位置

```
plugins/health.http/
├── plugin.json
└── bin/
    ├── libshield_health_http.dll
    ├── libshield_health_http.so
    └── libshield_health_http.dylib
```

### 运行时依赖

- **boost::beast / boost::asio**：通过 vcpkg 静态链接到共享库。
- 无 OpenSSL 依赖。
- **不链接 shield_net**：插件作为独立的叶子共享库，避免与 host 的静态 `shield_net` 符号冲突。

### 与 Kubernetes 集成

- 8086 端口应在 `Pod` spec 的 `ports` 段显式声明，便于 `kubectl describe` 看清。
- 通过 `Service` 暴露给集群内监控（如 Prometheus blackbox exporter 探测 /ready）。
- 容器 SIGTERM 后 kubelet 会先标记 readiness 失败（因为进程关闭 acceptor），再等 `terminationGracePeriodSeconds` 后发 SIGKILL。

### 多实例

同一个 Shield 进程可以创建多个 `health.http` 实例（不同端口），例如内部探针（8086）与管理面探针（8088）分开，避免运维误触。每个实例维护独立的检查项列表。

## 相关链接

- [插件系统](/plugin-system) — 接口模型、ABI 契约
- [插件参考索引](/plugins/) — 全部官方插件
- [Kubernetes Pod health checks](https://kubernetes.io/docs/concepts/configuration/liveness-readiness-startup-probes/) — liveness/readiness/startup probe 文档
- [boost::beast HTTP server 礓例](https://www.boost.org/doc/libs/release/libs/beast/doc/html/beast/quick_start/http_server.html)
