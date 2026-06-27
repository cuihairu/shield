# Plugin Pool Stats

> 状态：**草案 / proposal——尚未冻结。**
>
> 本接口尚未经真实插件验证。**冻结条件**：至少一个 SQL 插件（mysql / postgresql / sqlite）和一个 Redis 或 Mongo 插件能用本 vtable 表达真实池指标，且 host 采集 + ops/Prometheus 消费三方需求跑通。冻结前字段与签名可能调整。

## 动机

Shield 的数据访问是纯插件架构：每个数据库/Redis 插件在内部维护自己的连接池（见 [数据访问架构](runtime-data.md)、`include/shield/plugin/database.h`）。池逻辑高内聚、host 不耦合驱动，但代价是**池指标分散**——每个插件各自管理 `pool_size`，host 无法统一观测"哪个池快满了""哪个池在频繁驱逐"。

`shield.pool.stats.v1` 解决这个问题：插件**可选**实现一个 stats vtable，host 经统一 API 采集所有 instance 的池快照，供未来的 ops/metrics 消费端使用。

设计参考 Envoy 的 per-protocol ConnPool 抽象（框架定义接口、各实现自治）与 mongocxx 自带池的思路——池逻辑留在驱动/插件，观测接口统一抽出。

## 接口定义

接口名 `shield.pool.stats.v1`，与 `shield.database.v1` / `shield.cache.v1` 等平级。插件在 instance 上**可选**提供。**host 以 manifest 为权威发现支持者**，再用 `get_interface("shield.pool.stats.v1")` 取 vtable 校验——不并存两套发现模型。

下述 vtable 头面向 **C++ 插件/host**（沿用 `database.h` 的 `static constexpr interface_name` 形态，Shield 所有插件均为 C++）；struct 内存布局 ABI 稳定、跨编译器/DLL 兼容，但头本身是 C++（含 `constexpr`），**不承诺纯 C `#include` 兼容**。

```cpp
#define SHIELD_POOL_STATS_INTERFACE "shield.pool.stats.v1"

// Pool snapshot. All fields are point-in-time instantaneous values or
// monotonically increasing counters at the moment of the call.
//
// ABI versioning: `struct_size` MUST be the first field. The host MUST
// sentinel-fill the entire `out` first (gauges/counters = -1 unknown,
// last_error_epoch_ms = -1 unknown) and set struct_size = sizeof(shield_pool_stats)
// before calling. The plugin fills only the prefix it recognizes; fields
// added at the tail in future versions stay backward compatible (the host's
// sentinel survives if an older plugin leaves them unset).
//
// Unknown semantics: every instantaneous gauge may be -1 to mean
// "unknown / not applicable" (e.g. a driver that does not expose idle count).
// Cumulative counters use -1 the same way; 0 means "zero events", which is
// distinct from unknown.
struct shield_pool_stats {
    uint32_t struct_size;   // == sizeof(shield_pool_stats); MUST be first

    // --- Capacity & usage (instantaneous; -1 = unknown) ---
    int32_t  max_size;     // configured pool_size cap; 0 = unbounded, -1 = unknown
    int32_t  size;         // connections held = idle + in_use; -1 = unknown
    int32_t  idle;         // idle, available; -1 = unknown
    int32_t  in_use;       // checked out, in use; -1 = unknown

    // --- Waiting (gauge -1 = unknown; counter -1 = not tracked) ---
    int32_t  waiters;               // callers blocked on acquire; -1 = unknown
    int64_t  acquire_timeout_total; // cumulative acquire timeouts; -1 = unknown

    // --- Lifecycle (cumulative, monotonic; -1 = not tracked) ---
    int64_t  acquire_total;   // successful acquires
    int64_t  create_total;    // connections created
    int64_t  destroy_total;   // connections destroyed (incl. evicted)
    int64_t  eviction_total;  // connections evicted by health check
    int64_t  health_check_failures_total; // cumulative health-check failures

    // --- Last error ---
    int64_t  last_error_epoch_ms; // epoch ms of last pool-level error; 0 = none, -1 = unknown
};

struct shield_pool_stats_v1 {
    static constexpr const char* interface_name = SHIELD_POOL_STATS_INTERFACE;
    uint32_t struct_size;
    // Fill *out (honoring out->struct_size). Return code listed below.
    int (*get_stats)(struct shield_plugin_instance_v1* self,
                     struct shield_pool_stats* out);
};
```

host 必须校验 vtable 的 `struct_size` 至少覆盖 `get_stats` 字段，且 `get_stats` 非 NULL。若 manifest **未声明** stats → 视为不支持，跳过；若 manifest **已声明**但 vtable 无效 → 契约违反，**start 失败**（fail fast）。

### 返回码

| 返回值 | 含义 | host 行为 |
|--------|------|-----------|
| `0` | ok | 使用 `out` 快照 |
| `1` | unavailable | 池未初始化等暂时状态；记"暂无数据"，不告警 |
| `2` | unsupported_state | 当前状态无法采集（如正在关闭）；同上 |
| `<0` | internal_error | 采集本身失败；记错误/告警，与"暂无数据"区分 |

未识别的正返回值（`>2`）按 `unsupported_state` 处理；区间 `[3, N]` 保留给未来扩展的"非错误"状态码，负值统一为 internal_error。

### 字段语义

| 字段 | 类型 | 语义 |
|------|------|------|
| `struct_size` | — | ABI 版本化首字段，host 设为 `sizeof(shield_pool_stats)` |
| `max_size` | 瞬时 | 配置的 `pool_size` 上限；0 = 无界，-1 = 未知 |
| `size` | 瞬时 | 当前持有连接总数，正常 = `idle + in_use`；-1 = 未知 |
| `idle` / `in_use` | 瞬时 | 空闲可用 / 已借出；-1 = 驱动不暴露 |
| `waiters` | 瞬时 | 当前阻塞等待 acquire 的数量；-1 = 未知 |
| `acquire_timeout_total` | 累计 | acquire 超时失败累计；-1 = 不跟踪 |
| `acquire_total` | 累计 | 成功 acquire 累计 |
| `create_total` / `destroy_total` | 累计 | 新建 / 销毁连接累计 |
| `eviction_total` | 累计 | 健康检查失败被驱逐累计 |
| `health_check_failures_total` | 累计 | 健康检查失败累计（连接通常一旦不健康即被驱逐，故用累计而非"当前不健康数"） |
| `last_error_epoch_ms` | 瞬时 | 最近 pool 级错误 epoch 毫秒；0 = 无，-1 = 未知 |

**快照语义**：

- **线程安全**：`get_stats` 必须线程安全，可被 host 任意线程调用。
- **低开销、不阻塞业务**：**不得做 IO，不得等待连接池 acquire/release 路径**；应为 bounded low-latency（目标微秒级），只读原子计数/瞬时值，不加业务争用的锁。
- **字段一致性**：瞬时 gauge 之间是 **best-effort**（非原子整体快照，`size` 与 `idle+in_use` 可能瞬态不等）；累计计数器各自原子读。消费端按此处理。
- **counter reset**：所有累计值在**单个 plugin instance 生命周期内单调递增**；instance 重启后允许归零，消费端应按 instance 生命周期识别 reset 并重新基线。

## manifest 声明与发现

实现本接口的插件**必须在 `manifest.yaml` 的 `provides` 增加第二项**：

```yaml
provides:
  - interface: shield.database.v1     # 业务接口（原有）
    capabilities: [sql, transactions]
  - interface: shield.pool.stats.v1   # 观测接口（新增）
    capabilities: []                  # 无 capability，仅声明"我实现了 stats"
```

这样 manifest catalog、配置校验、文档生成与运行时 `get_interface` 校验四者一致。host 以 manifest 静态发现"哪些插件支持 stats"，再用 runtime probe 校验声明是否真实。stats 是纯观测接口，不参与 binding 解析，不影响业务调用路径。

**一致性校验**（manifest 为权威事实源，catalog/start 阶段）：

- manifest 声明了 `shield.pool.stats.v1`，但运行时 `get_interface` 返回 NULL → **start 失败**（声明即必须实现，fail fast）。
- `get_interface` 非 NULL 但 manifest 未声明 → **不采集**（manifest 为准）；host 记 debug 日志便于排查实现不一致，但不进入 stats 结果，保证 catalog/校验/文档/probe 四者一致。

## 实现插件

**有可观测连接池的插件应实现本接口**；当驱动无法提供某些字段时，可将该字段置 `-1`（unknown），或干脆不实现该接口（`get_interface` 返回 NULL）——**不为凑接口而伪造数据**。

预期会实现的：mysql / postgresql（SQL 连接池）、mongodb（mongocxx client 池）、cache.redis / queue.redis / leaderboard.redis（redis 连接池）。各插件按自身真实能力暴露字段，驱动不暴露的字段置 `-1`（unknown）。

**SQLite 不实现本接口**：SQLite 是嵌入式引擎，**没有连接池**（每次调用打开一个文件连接，见 `plugins/sqlite/shield_db_sqlite.cpp`），不属于"有可观测连接池的插件"。其余无池插件（health / matchmaking / auth / metrics 等）同样不实现。

## host 侧采集

`PluginHost` 只遍历 manifest 声明了 `shield.pool.stats.v1` 的 started instances，对每个调 `get_interface("shield.pool.stats.v1")` 校验并取得 vtable，再调用 `get_stats`。聚合 API（本文档定义签名，实现后续）：

```cpp
// Per-instance collection outcome. status encodes the get_stats return code,
// so ops can tell "no data" (unavailable/unsupported_state) from "collection
// failed" (error).
enum class PoolStatsStatus {
    ok,                // stats valid
    unavailable,       // get_stats returned 1 (pool not initialized, etc.)
    unsupported_state, // get_stats returned 2
    error,             // get_stats returned <0; see error_code/message
};

struct PoolStatsResult {
    std::string instance_id;    // e.g. "primary_db"
    std::string plugin_id;      // e.g. "database.mysql"
    std::string pool_name;      // pool within the instance; "main" for v1
    PoolStatsStatus status;     // ok → read stats; else see error_*
    int raw_status_code;        // 原始 get_stats 返回值；status 把 >2 折叠为
                                // unsupported_state，此字段保留原始值供未来
                                // 扩展状态诊断
    std::string error_code;     // host-generated, mapped from get_stats return
                                // code (v1 ABI has no plugin error channel),
                                // e.g. "pool_stats_unavailable" /
                                // "pool_stats_internal_error"
    std::string error_message;  // host-generated generic description
    shield_pool_stats stats;    // valid only when status == ok
};

// Append one PoolStatsResult per instance that implements the interface
// (declared in manifest, cross-checked via get_interface). Instances without
// the interface are not listed. v1: one result per instance (pool_name "main").
// Returns false only on hard errors (e.g. host not started).
bool PluginHost::collect_pool_stats(std::vector<PoolStatsResult>& out);
```

> **错误信息来源**：v1 的 `get_stats` 只返回 int 返回码，没有插件侧错误字符串通道。因此 `error_code` / `error_message` 由 **host 从返回码映射生成**（generic，非插件提供）。若未来需要插件细粒度错误，再给 `get_stats` 增加 `shield_error_v1* err` 出参（YAGNI，v1 不做）。

## 已知限制（v1）与未来方向

- **单 pool per instance**：v1 假设一个 instance 一个主池，返回单条快照。读写分离主从池、Redis pub/sub 专用连接、multi-tenant / replica pool 等多池场景留待 **v2**：届时 `get_stats` 改为枚举多个命名池，`PoolStatsResult.pool_name` 已为此预留。
- **只读观测**：不提供控制能力（手动驱逐、缩容）；若需要，后续单独立接口（YAGNI，当前不做）。

## 消费者（deferred）

`shield_ops` 官方可选模块（含 metrics exporter、`/ops/*` 端点）目前仍是未实现的 deferred 方向（见 [运维语义](runtime-ops.md)）。因此本接口**先定义 ABI 与 host 采集 API，消费端留待 ops module 落地时接入**，预期形态：

- `GET /ops/pools` —— 返回所有 instance 的池快照 JSON。
- Prometheus exporter —— 将累计计数导出为 `shield_pool_*` 指标。**unknown 字段（-1）不导出对应指标**，避免 -1 进入告警计算；若需暴露字段可用性，可导出配套 `*_known` gauge（1=有效 / 0=unknown）。

在 ops module 落地前，C++ 侧可通过 `collect_pool_stats` 直接诊断。

## 与其他接口的关系

- **不替代** `database.v1` 等业务接口——那些仍是业务调用入口。
- **可选**——不实现不影响插件正常工作，只影响可观测性。

## 参考

- [数据访问架构](runtime-data.md) —— shield_data 现状与插件数据架构总览。
- [插件系统 v1](plugin-system.md) —— 插件 ABI、instance、get_interface 机制。
- Envoy [Connection Pooling](https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/upstream/connection_pooling) —— per-protocol 池抽象的工业参考。
