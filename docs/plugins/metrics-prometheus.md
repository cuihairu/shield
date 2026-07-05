# metrics.prometheus

> Prometheus 文本格式指标导出，实现 `shield.metrics.v1` 接口，内嵌 boost::beast HTTP 服务端，暴露 `/metrics` 端点供 Prometheus 拉取。

## 包信息

- **包 ID**: `metrics.prometheus`
- **接口**: [`shield.metrics.v1`](/plugin-system#interface-model)
- **Capabilities**: `counter`, `gauge`, `histogram`
- **版本**: 1.0.0
- **CMake 选项**: `SHIELD_BUILD_PLUGIN_METRIC`
- **源码**: `plugins/metric_prometheus/`
- **依赖**: boost::beast、boost::asio（HTTP 服务端）；不依赖 OpenSSL

## 构建启用

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_METRIC=ON
cmake --build build --config Release
```

注意：源码目录名是 `metric_prometheus`（单数 metric），与包 ID `metrics.prometheus`（复数）的命名约定保持一致。产物部署到 `plugins/metrics.prometheus/bin/`。

## 配置 Schema

| 字段 | 类型 | 必填 | 默认值 | 范围 | 说明 |
| --- | --- | --- | --- | --- | --- |
| `bind_address` | string | 否 | `0.0.0.0` | — | HTTP 监听地址 |
| `port` | integer | 否 | 8087 | 1–65535 | HTTP 监听端口 |
| `path` | string | 否 | `/metrics` | — | Prometheus scrape 路径 |

`app.yaml` 示例：

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: metrics.main
      package: metrics.prometheus
      required: true
      config:
        bind_address: "0.0.0.0"
        port: 8087
        path: "/metrics"
  bindings:
    metrics.default: metrics.main
```

## 接口契约

实现 `include/shield/plugin/metrics.h` 中的 `shield_metrics_v1` vtable。内部用 `std::map<SeriesKey, ...>` 维护 counter / gauge / histogram 三类时间序列，全部共享一个互斥锁。

### connect / disconnect

```c
struct shield_metrics_session* (*connect)(const struct shield_metrics_config* cfg,
                                          char* err_buf, int err_buf_size);
void (*disconnect)(struct shield_metrics_session* session);
```

`metrics.prometheus` 采用拉取（pull）模型：HTTP 监听器在 `instance->start()` 阶段启动，与 vtable 的 `connect` 解耦。`connect` 直接返回 `nullptr`，`disconnect` 为 no-op。Session 句柄即 instance 本身。

### shield_metric_point

```c
enum shield_metric_type {
    SHIELD_METRIC_COUNTER   = 0,
    SHIELD_METRIC_GAUGE     = 1,
    SHIELD_METRIC_HISTOGRAM = 2,
    SHIELD_METRIC_TIMER     = 3,
};

struct shield_metric_point {
    const char* name;
    enum shield_metric_type type;
    double value;
    const char* const* label_keys;
    const char* const* label_values;
    int label_count;
    int64_t timestamp_ms;  // 0 = now
};
```

| 字段 | 说明 |
| --- | --- |
| `name` | 指标名，Prometheus 命名约定（`[a-zA-Z_:][a-zA-Z0-9_:]*`） |
| `type` | 决定写入 counter / gauge / histogram 哪张表 |
| `value` | 数值 |
| `label_keys` / `label_values` | 平行数组，长度由 `label_count` 决定 |
| `timestamp_ms` | 预留；当前实现忽略，导出时由 Prometheus server 端打时间戳 |

`SHIELD_METRIC_TIMER` 在导出时与 `HISTOGRAM` 等价（都渲染为 `<name>_sum` + `<name>_count`）。

### record / record_batch

```c
int (*record)(struct shield_metrics_session* session,
              const struct shield_metric_point* point);
int (*record_batch)(struct shield_metrics_session* session,
                    const struct shield_metric_point* points, int count);
```

通用入口，根据 `point->type` 分发到下列三个具体方法。`record_batch` 是循环调用 `record` 的语义，但每条单独加锁（避免长临界区影响 HTTP scrape 响应）。

### counter_inc

```c
int (*counter_inc)(struct shield_metrics_session* session,
                   const char* name, double value,
                   const char* const* label_keys,
                   const char* const* label_values, int label_count);
```

把 `(name, labels)` 对应的 counter 累加 `value`。counter 是单调递增语义，调用方应保证 `value >= 0` 且不会回退。

### gauge_set

```c
int (*gauge_set)(struct shield_metrics_session* session,
                 const char* name, double value,
                 const char* const* label_keys,
                 const char* const* label_values, int label_count);
```

把 `(name, labels)` 对应的 gauge 直接覆盖为 `value`。gauge 可任意上下波动，适合表示当前连接数、队列长度、内存占用等瞬时值。

### histogram_observe

```c
int (*histogram_observe)(struct shield_metrics_session* session,
                         const char* name, double value,
                         const char* const* label_keys,
                         const char* const* label_values, int label_count);
```

为 `(name, labels)` 观察 `value` 一次。内部维护 `sum` 与 `count` 两个累加器，导出时分别渲染为 `<name>_sum{labels}` 与 `<name>_count{labels}`。

注意：当前 v1 实现没有维护 bucket 直方图（仅 sum/count），因此无法在 Prometheus 端计算 p99/p95 分位数。如需完整 bucket，需在插件层扩展或后续 v1.1 升级。

### flush

```c
int (*flush)(struct shield_metrics_session* session);
```

No-op。pull 模型下数据始终在内存，scrape 时实时渲染；不需要主动推送。

## Prometheus 文本格式导出

HTTP 服务端在 `start()` 阶段启动，监听 `bind_address:port`，对每个连接同步读取请求、写入响应。仅响应 `GET <path>`（默认 `/metrics`），其它路径返回 404。

响应体示例：

```text
# TYPE shield_http_requests_total counter
shield_http_requests_total{method="GET",path="/api/login",status="200"} 1234
shield_http_requests_total{method="POST",path="/api/match",status="200"} 567
# TYPE shield_active_connections gauge
shield_active_connections 42
# TYPE shield_request_duration_seconds histogram
shield_request_duration_seconds_sum{handler="login"} 12.34
shield_request_duration_seconds_count{handler="login"} 1234
```

响应头：

- `Content-Type: text/plain; version=0.0.4`
- `Server: shield.metrics.prometheus`
- 状态码：200（命中路径）/ 404（其它）

标签（labels）规则：

- 标签键值对在内部用排好序的 `vector<pair<string,string>>` 规范化，因此同一组标签的不同插入顺序会落到同一 series。
- 标签值会原样转义到双引号字符串中（当前实现不做 Prometheus 转义，调用方应避免在 label 中包含 `\`、`"`、换行）。
- 空标签集合渲染为不带 `{}` 的纯指标名。

## 使用示例

### C++（通过 binding）

记录 HTTP 请求计数 + 请求耗时直方图：

```cpp
#include "shield/plugin/metrics.h"

const shield_metrics_v1* metrics = /* 从 metrics.default binding 拿到 */;
shield_metrics_session* session = /* 从 host 拿到对应实例句柄 */;

// 1. Counter: 每收到一个请求 +1，带 method/path/status 标签
const char* lk[] = { "method", "path", "status" };
const char* lv[] = { "POST", "/api/match", "200" };
metrics->counter_inc(session, "shield_http_requests_total", 1.0, lk, lv, 3);

// 2. Gauge: 维护当前在线玩家数
metrics->gauge_set(session, "shield_active_players",
                   static_cast<double>(online_count), nullptr, nullptr, 0);

// 3. Histogram: 观察一次请求耗时（秒）
const char* hlk[] = { "handler" };
const char* hlv[] = { "login" };
metrics->histogram_observe(session, "shield_request_duration_seconds",
                           elapsed_seconds, hlk, hlv, 1);

// 4. 批量记录
shield_metric_point batch[2] = {};
batch[0].name = "shield_events_processed_total";
batch[0].type = SHIELD_METRIC_COUNTER;
batch[0].value = 1;
batch[1].name = "shield_queue_depth";
batch[1].type = SHIELD_METRIC_GAUGE;
batch[1].value = queue_size;
metrics->record_batch(session, batch, 2);
```

Prometheus scrape 配置（`prometheus.yml`）：

```yaml
scrape_configs:
  - job_name: "shield"
    static_configs:
      - targets: ["my-host:8087"]
    metrics_path: /metrics
    scrape_interval: 15s
```

### Lua（规划中）

`metrics.prometheus` 当前 `register_lua` 是空实现。计划中的 namespace 约定：

```lua
-- 未来将注册到 shield.metrics.prometheus
local m = shield.metrics.prometheus("metrics.main")
m:inc("shield_http_requests_total", 1, { method = "GET", status = "200" })
m:set("shield_active_players", 42)
m:observe("shield_request_duration_seconds", 0.123, { handler = "login" })
```

## 平台特性

### 默认端口与路径

默认 `0.0.0.0:8087/metrics`。端口选择避开 8086（`health.http` 占用）以及 9090（Prometheus server 自身）。多实例部署需要显式分配不同端口。

### HTTP 服务端实现

内嵌 boost::beast 同步 accept 循环，单线程串行处理请求。Prometheus scrape 频率通常为 15–60 秒一次，QPS 极低，单线程足以承载。监听 socket 设置 `SO_REUSEADDR`，便于进程重启时快速绑回端口。

### 单线程 accept 循环

```text
while running:
    socket = accept()        # 阻塞
    handle_session(socket)   # 同步 read + write
```

每个请求在同一 IO 线程内完成读取-渲染-写入，不引入工作线程池。所有 counter/gauge/histogram 操作通过 `std::mutex` 保护，与 scrape 的 render 调用互斥。

### 标签规范化

`(name, labels)` 是 series 的复合 key。labels 在写入前先排序去重（按 key 字典序），因此以下两次调用会落到同一 series：

```cpp
m->counter_inc(s, "req", 1, {"b","a"}, {"2","1"}, 2);
m->counter_inc(s, "req", 1, {"a","b"}, {"1","2"}, 2);
```

### Counter / Gauge / Histogram 类型选择

| 场景 | 类型 | 说明 |
| --- | --- | --- |
| 累计请求数、错误数、事件数 | counter | 单调递增，Prometheus 通过 `rate()` 计算速率 |
| 当前连接数、队列深度、温度 | gauge | 可上下波动 |
| 请求耗时、响应大小分布 | histogram | 提供 sum/count；bucket 扩展待 v1.1 |

### Pull 模型

不支持 push（`push_interval_seconds` 在配置 schema 中未暴露）。所有指标通过 Prometheus 主动 scrape 获取。这与 Prometheus 官方推荐一致，也简化了网络与防火墙配置。

## 错误处理

| 场景 | 返回值 | 说明 |
| --- | --- | --- |
| `record` / `counter_inc` / `gauge_set` / `histogram_observe` 参数为空 | `-1` | `session`、`name` 为 `nullptr` |
| `record_batch` 参数为空 | `-1` | `points` 为 `nullptr` |
| 正常记录 | `0` | 包括未识别 label 键值对（被忽略） |
| 端口绑定失败（`start` 阶段） | `1` 并填 `plugin.init.failed` | 通常是端口被占用或权限不足 |

counter/gauge/histogram 操作本身不会失败；返回值非 0 仅表示参数校验问题。

## 部署

### 二进制位置

```
plugins/metrics.prometheus/
├── manifest.yaml
└── bin/
    ├── libshield_metric_prometheus.dll
    ├── libshield_metric_prometheus.so
    └── libshield_metric_prometheus.dylib
```

### 运行时依赖

- **boost::beast / boost::asio**：通过 vcpkg 静态链接到插件共享库，部署侧不需要额外 boost DLL/SO。
- 无 OpenSSL 依赖。

### 与 Prometheus 集成

1. 在 `prometheus.yml` 的 `scrape_configs` 添加 target 指向 Shield 进程的 8087 端口。
2. `metrics_path` 与插件配置的 `path` 一致（默认 `/metrics`）。
3. 推荐配置 `honor_labels: false`（Shield 导出的指标已带正确命名）。
4. 生产环境建议用 nginx/envoy 反代 8087，配合 TLS 与基本认证。

### 与 Grafana 集成

Prometheus 作为数据源，Grafana 直接基于导出的指标名做查询。常用 PromQL：

```txt
rate(shield_http_requests_total[5m])                          # QPS
shield_active_players                                         # 在线人数
rate(shield_request_duration_seconds_sum[5m])
  / rate(shield_request_duration_seconds_count[5m])           # 平均耗时
```

### 与 Kubernetes 集成

- 用 `Service` + `ServiceMonitor`（Prometheus Operator）声明 scrape。
- `Pod` 注解 `prometheus.io/port: "8087"`、`prometheus.io/path: "/metrics"` 可被旧版 Prometheus 自动发现。
- 容器暴露 8087 端口即可，不需要 hostNetwork。

## 相关链接

- [插件系统](/plugin-system) — 接口模型、ABI 契约
- [插件参考索引](/plugins/) — 全部官方插件
- [Prometheus exposition format](https://prometheus.io/docs/instrumenting/exposition_formats/) — 文本格式规范
- [Prometheus best practices for naming](https://prometheus.io/docs/practices/naming/) — 指标命名约定
- [boost::beast 文档](https://www.boost.org/doc/libs/release/libs/beast/) — HTTP 服务端实现基础
