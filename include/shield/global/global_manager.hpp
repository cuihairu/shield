// [SHIELD_GLOBAL] Process-wide global-capability store (P0).
// See docs/runtime-global.md: global data + local cache, locks (mutex /
// rwlock / spinlock; the distributed_* twins share the same seams),
// leaderboards, queues (normal / delay / reliable) and the cron/interval/
// once scheduler. Plain C++ singletons behind per-domain mutexes — no CAF,
// no sol2; the Lua facade lives in shield_lua, task callbacks are injected
// as std::function so shield_global never links shield_lua.
//
// P0 backend is process memory (the runtime-global.md Redis backend needs
// atomic primitives the data-plugin vtables do not expose yet; the seam is
// this class, so a cross-process backend can slot in without touching the
// Lua surface).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace shield::global {

/// Module-owned configuration (the `global` config section). Parsed by the
/// module itself; core config never grows global fields.
struct GlobalConfig {
    std::uint64_t cache_max_size = 10000;
    std::uint64_t cache_default_ttl_ms = 60000;
    std::uint64_t scheduler_tick_ms = 250;

    /// Reads the `global` section from the global config. Returns false
    /// with `error` set when a present section fails validation.
    static bool from_global_config(GlobalConfig* out, std::string* error);
};

/// Independent validation so bootstrap can fail fast with a message that
/// names the offending field (OMOD-GL-008).
bool validate_global_config(const GlobalConfig& config, std::string* error);

/// ---------------------------------------------------------------------------
/// Cron expressions (5 fields: minute hour dom month dow, UTC, minute
/// granularity). Each field is a bitset over its domain.
/// ---------------------------------------------------------------------------
struct CronFields {
    std::uint64_t minute = 0;  // bit m set => minute m allowed (0-59)
    std::uint64_t hour = 0;    // bit h set => hour h allowed (0-23)
    std::uint64_t dom = 0;     // bit d set => day-of-month d (1-31, bit d)
    std::uint64_t month = 0;   // bit m set => month m (1-12, bit m)
    std::uint64_t dow = 0;     // bit w set => weekday w (0=Sunday, bit w)
};

/// Parses `*`, `n`, `a-b`, `*/s`, `a-b/s` and comma lists per field.
/// Returns false with `error` set on a malformed expression.
bool parse_cron(const std::string& expression, CronFields* out,
                std::string* error);

/// Next matching wall-clock ms strictly after `from_ms`; 0 when no match
/// within the ~2 year scan horizon. dom/dow follow standard cron: when
/// both are restricted the day matches if EITHER side allows it.
std::uint64_t cron_next(const CronFields& fields, std::uint64_t from_ms);

/// ---------------------------------------------------------------------------
/// Locks. `owner` is an opaque token minted by the Lua facade (per lock
/// object); reentrancy keys on it. TTL expiry is lazy: an expired lock is
/// taken over by the next acquirer.
/// ---------------------------------------------------------------------------
enum class LockStatus {
    kOk,        /// acquired (fresh or reentrant) / released / extended
    kBusy,      /// another owner holds a live lock
    kNotOwner,  /// release/extend by a non-holder (or no lock at all)
};

struct LockInfo {
    bool exists = false;
    std::string owner;
    std::uint64_t count = 0;  /// reentrant hold count
    std::uint64_t acquired_at_ms = 0;
    std::uint64_t ttl_remaining_ms = 0;  /// 0 = no TTL / no lock
};

struct RwLockInfo {
    bool exists = false;
    std::string write_owner;
    std::uint64_t write_count = 0;
    std::uint64_t write_ttl_remaining_ms = 0;
    std::size_t readers = 0;  /// distinct reader tokens currently holding
};

/// ---------------------------------------------------------------------------
/// Leaderboards: score desc, uid asc on ties (Redis ZSET order).
/// ---------------------------------------------------------------------------
struct RankEntry {
    std::string uid;
    double score = 0.0;
    std::uint64_t rank = 0;  // 1-based
};

struct RankAround {
    std::vector<RankEntry> above;
    std::optional<RankEntry> target;
    std::vector<RankEntry> below;
};

/// ---------------------------------------------------------------------------
/// Reliable queue delivery handle.
/// ---------------------------------------------------------------------------
enum class NackResult {
    kRequeued,  /// back on the pending set after the retry delay
    kDead,      /// retries exhausted: moved to the dead letter set
    kNotFound,  /// unknown/already-acked delivery id
};

struct ReliableDelivery {
    std::uint64_t delivery_id = 0;
    std::string payload;
};

struct DeadEntry {
    std::uint64_t delivery_id = 0;
    std::string payload;
    std::uint64_t retries = 0;
};

/// ---------------------------------------------------------------------------
/// Scheduler tasks.
/// ---------------------------------------------------------------------------
struct SchedInfo {
    std::string name;
    std::string type;      // "cron" | "interval" | "once"
    std::string schedule;  // cron expr / interval ms / delay ms
    std::string service_id;
    std::uint64_t next_run_ms = 0;
    std::uint64_t last_run_ms = 0;
    std::uint64_t run_count = 0;
    bool paused = false;
    bool done = false;
};

/// Task delivery hook: invoked without any manager lock held on the tick
/// thread (or the trigger caller's thread). Return false when the owning
/// service is gone — the task is then dropped (server-watch kGone twin).
using TaskFireFn =
    std::function<bool(const std::string& service_id, const std::string& task)>;

/// ---------------------------------------------------------------------------
/// Rate limiters: token bucket (default) or exact sliding window, per
/// (limiter, key). P0 keeps the buckets in-process; the Redis quota sync of
/// runtime-global.md is the Phase 2+ backend shape.
/// ---------------------------------------------------------------------------
struct RateLimitConfig {
    double rate = 100.0;               /// refill tokens per second (bucket)
    double burst = 200.0;              /// bucket capacity (bucket)
    bool sliding = false;              /// true: exact window, ignore rate/burst
    std::uint64_t window_ms = 60000;   /// window size (sliding)
    std::uint64_t max_requests = 100;  /// per-window cap (sliding)
};

struct RateLimitResult {
    bool allowed = false;
    double remaining = 0.0;            /// tokens (bucket) or slots (window)
    std::uint64_t retry_after_ms = 0;  /// 0 when allowed
};

/// Process-wide store. Thread-safe; every domain keeps its own mutex so a
/// slow scheduler tick never blocks data traffic.
class GlobalManager {
public:
    explicit GlobalManager(GlobalConfig config);
    ~GlobalManager();

    GlobalManager(const GlobalManager&) = delete;
    GlobalManager& operator=(const GlobalManager&) = delete;

    static GlobalManager* global();
    static void set_global(GlobalManager* manager);

    const GlobalConfig& config() const { return config_; }

    /// Wall-clock ms (single time source for TTLs / queues / scheduler).
    static std::uint64_t now_ms();

    void set_task_fire_fn(TaskFireFn fn);

    /// Starts the scheduler tick thread. Idempotent.
    void start();

    /// Stops the tick thread and drops the injected callbacks (bootstrap
    /// teardown; also run by the destructor).
    void stop();

    // ---- global data (JSON text values, optional TTL) ----
    bool data_get(const std::string& key, std::string* out);
    void data_set(const std::string& key, std::string value,
                  std::uint64_t ttl_ms);
    bool data_delete(const std::string& key);
    bool data_incr_by(const std::string& key, std::int64_t delta,
                      std::int64_t* out, std::string* error);
    std::vector<std::optional<std::string>> data_mget(
        const std::vector<std::string>& keys);
    /// Returns false when any key/value pair is malformed; no partial write.
    bool data_mset(const std::vector<std::pair<std::string, std::string>>& kvs,
                   std::uint64_t ttl_ms, std::string* error);
    std::size_t data_size();

    // ---- local cache (LRU, miss-fills from the data domain) ----
    /// `ttl_ms` 0 falls back to the configured default.
    bool cache_get(const std::string& key, std::uint64_t ttl_ms,
                   std::string* out);
    void cache_invalidate(const std::string& key);
    void cache_clear();
    std::size_t cache_size();
    std::uint64_t cache_hits();
    std::uint64_t cache_misses();

    // ---- locks: exclusive (mutex + spinlock registries; the
    // distributed_mutex facade shares the mutex registry) ----
    /// `ttl_ms` 0 = no expiry (held until release).
    LockStatus mutex_acquire(const std::string& registry,
                             const std::string& name, const std::string& owner,
                             std::uint64_t ttl_ms);
    LockStatus mutex_release(const std::string& registry,
                             const std::string& name, const std::string& owner);
    bool mutex_extend(const std::string& registry, const std::string& name,
                      const std::string& owner, std::uint64_t ttl_ms);
    LockInfo mutex_info(const std::string& registry,
                        const std::string& name) const;
    std::size_t mutex_registry_size(const std::string& registry) const;

    // ---- rwlocks (distributed_rwlock shares this registry) ----
    LockStatus rw_write_acquire(const std::string& name,
                                const std::string& owner, std::uint64_t ttl_ms);
    LockStatus rw_write_release(const std::string& name,
                                const std::string& owner);
    bool rw_write_extend(const std::string& name, const std::string& owner,
                         std::uint64_t ttl_ms);
    LockStatus rw_read_acquire(const std::string& name,
                               const std::string& owner);
    LockStatus rw_read_release(const std::string& name,
                               const std::string& owner);
    RwLockInfo rwlock_info(const std::string& name) const;
    std::size_t rwlock_count() const;

    // ---- leaderboards ----
    void rank_update(const std::string& board, const std::string& uid,
                     double score);
    void rank_mupdate(
        const std::string& board,
        const std::vector<std::pair<std::string, double>>& updates);
    std::optional<double> rank_score(const std::string& board,
                                     const std::string& uid);
    std::optional<std::uint64_t> rank_position(const std::string& board,
                                               const std::string& uid);
    std::vector<RankEntry> rank_top(const std::string& board, std::size_t n);
    /// 1-based inclusive rank window; out-of-range bounds clamp.
    std::vector<RankEntry> rank_range(const std::string& board,
                                      std::uint64_t from, std::uint64_t to);
    /// Score window [lo, hi], rank order.
    std::vector<RankEntry> rank_range_by_score(const std::string& board,
                                               double lo, double hi);
    RankAround rank_around(const std::string& board, const std::string& uid,
                           std::size_t n);
    std::size_t rank_count(const std::string& board);
    bool rank_remove(const std::string& board, const std::string& uid);
    void rank_clear(const std::string& board);
    std::size_t rank_board_count();
    std::size_t rank_total_members();

    // ---- normal queues (FIFO) ----
    void queue_push(const std::string& name, std::string payload);
    void queue_push_batch(const std::string& name,
                          const std::vector<std::string>& payloads);
    bool queue_pop(const std::string& name, std::string* out);
    std::size_t queue_length(const std::string& name);
    void queue_purge(const std::string& name);
    std::size_t queue_count();

    // ---- delay queues (ready_at ordered, FIFO within a timestamp) ----
    void delay_push(const std::string& name, std::string payload,
                    std::uint64_t delay_ms);
    void delay_push_at(const std::string& name, std::string payload,
                       std::uint64_t at_ms);
    bool delay_pop(const std::string& name, std::string* out);
    std::size_t delay_pending(const std::string& name);
    std::size_t delay_ready(const std::string& name);
    void delay_purge(const std::string& name);
    std::size_t delay_queue_count();

    // ---- priority queues (smaller priority pops first, FIFO within a
    // level; the P0 backend is the Redis ZSET twin's in-process shape) ----
    void priority_push(const std::string& name, std::string payload,
                       std::int64_t priority);
    bool priority_pop(const std::string& name, std::string* out);
    std::size_t priority_length(const std::string& name);
    void priority_purge(const std::string& name);
    std::size_t priority_queue_count();

    // ---- reliable queues (pop -> delivery handle -> ack/nack) ----
    /// Per-queue knobs; `max_retries` <= 0 keeps the default of 3.
    void reliable_configure(const std::string& name, int max_retries);
    void reliable_push(const std::string& name, std::string payload);
    bool reliable_pop(const std::string& name, ReliableDelivery* out);
    bool reliable_ack(const std::string& name, std::uint64_t delivery_id);
    NackResult reliable_nack(const std::string& name, std::uint64_t delivery_id,
                             std::uint64_t retry_ms);
    std::vector<DeadEntry> reliable_dead_range(const std::string& name,
                                               std::size_t from,
                                               std::size_t to);
    std::size_t reliable_dead_size(const std::string& name);
    void reliable_dead_purge(const std::string& name);
    std::size_t reliable_queue_count();
    std::size_t reliable_inflight(const std::string& name);

    // ---- scheduler ----
    /// `type` is one of "cron" | "interval" | "once"; `schedule` is the
    /// cron expression or the ms amount. A duplicate task name fails.
    bool sched_register(const std::string& type, const std::string& name,
                        const std::string& schedule,
                        const std::string& service_id, std::string* error);
    bool sched_remove(const std::string& name);
    bool sched_pause(const std::string& name);
    bool sched_resume(const std::string& name);
    /// Fires the task right now (delivery only; the schedule is untouched).
    bool sched_trigger(const std::string& name);
    std::optional<SchedInfo> sched_get(const std::string& name);
    std::vector<SchedInfo> sched_list();
    std::size_t sched_active_count();

    // ---- rate limiters ----
    /// (Re)sets the limiter's config and drops its per-key state, so a
    /// re-configure never leaves buckets sized for the old shape.
    void rate_limit_configure(const std::string& name,
                              const RateLimitConfig& config);
    /// Consumes `cost` tokens (bucket) or one slot (sliding; cost ignored).
    RateLimitResult rate_limit_allow(const std::string& name,
                                     const std::string& key, double cost);
    /// Query-only: refills/evicts virtually, mutates nothing.
    double rate_limit_remaining(const std::string& name,
                                const std::string& key);
    std::size_t rate_limit_key_count(const std::string& name);
    void rate_limit_purge(const std::string& name);

private:
    struct MutexEntry {
        std::string owner;
        std::uint64_t count = 0;
        std::uint64_t acquired_at_ms = 0;
        std::uint64_t expire_at_ms = 0;  // 0 = no TTL
    };
    struct RwLockEntry {
        std::string write_owner;
        std::uint64_t write_count = 0;
        std::uint64_t write_expire_at_ms = 0;
        std::map<std::string, std::uint64_t> readers;  // token -> hold count
    };
    struct DelayedItem {
        std::uint64_t id = 0;  // reliable delivery id; 0 for plain items
        std::string payload;
    };
    struct DeadItem {
        std::uint64_t delivery_id = 0;
        std::string payload;
        std::uint64_t retries = 0;
    };
    struct ReliableQueue {
        std::multimap<std::uint64_t, DelayedItem> pending;
        std::unordered_map<std::uint64_t, DeadItem> inflight;
        std::vector<DeadItem> dead;
        std::unordered_map<std::uint64_t, std::uint64_t>
            retry_counts;  // delivery id -> nacks so far
        std::uint64_t next_delivery_id = 1;
        int max_retries = 3;
    };
    struct SchedTask {
        SchedInfo info;
        CronFields cron;  // valid when type == "cron"
    };
    struct RateBucket {
        double tokens = 0;
        std::uint64_t last_refill_ms = 0;  // 0 = not initialized yet
    };
    struct RateWindow {
        std::deque<std::uint64_t> hits;  // ms timestamps inside the window
    };
    struct RateLimiter {
        RateLimitConfig config;
        std::unordered_map<std::string, RateBucket> buckets;
        std::unordered_map<std::string, RateWindow> windows;
    };

    // Shared helpers (each domain lock must already be held).
    bool mutex_try_acquire_locked(
        std::unordered_map<std::string, MutexEntry>& registry,
        const std::string& name, const std::string& owner,
        std::uint64_t ttl_ms);
    static bool mutex_live(const MutexEntry& entry, std::uint64_t now);
    std::vector<RankEntry> rank_sorted_locked(
        const std::unordered_map<std::string, double>& board);
    void fire_task(const SchedTask& task);
    void tick_loop();

    GlobalConfig config_;

    mutable std::mutex data_mutex_;
    std::unordered_map<std::string, std::uint64_t> data_expire_;  // key -> ts
    std::unordered_map<std::string, std::string> data_;
    std::list<std::pair<std::string, std::string>> cache_lru_;  // front = hot
    std::unordered_map<std::string,
                       std::list<std::pair<std::string, std::string>>::iterator>
        cache_index_;
    std::unordered_map<std::string, std::uint64_t> cache_expire_;
    std::uint64_t cache_hits_ = 0;
    std::uint64_t cache_misses_ = 0;

    mutable std::mutex locks_mutex_;
    std::unordered_map<std::string, MutexEntry> mutexes_;
    std::unordered_map<std::string, MutexEntry> spinlocks_;
    std::unordered_map<std::string, RwLockEntry> rwlocks_;

    mutable std::mutex ranks_mutex_;
    std::unordered_map<std::string, std::unordered_map<std::string, double>>
        ranks_;

    mutable std::mutex queues_mutex_;
    std::unordered_map<std::string, std::deque<std::string>> queues_;
    std::unordered_map<std::string, std::multimap<std::uint64_t, DelayedItem>>
        delay_queues_;
    std::unordered_map<std::string, ReliableQueue> reliable_queues_;
    /// name -> (priority -> FIFO levels); empty levels are erased on pop.
    std::unordered_map<std::string,
                       std::map<std::int64_t, std::deque<std::string>>>
        priority_queues_;

    mutable std::mutex sched_mutex_;
    std::unordered_map<std::string, SchedTask> tasks_;

    mutable std::mutex rate_limits_mutex_;
    std::unordered_map<std::string, RateLimiter> rate_limiters_;

    TaskFireFn task_fire_fn_;
    std::jthread tick_thread_;
    bool tick_running_ = false;
    std::condition_variable tick_cv_;
    std::mutex tick_control_mutex_;
};

}  // namespace shield::global
