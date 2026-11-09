// [SHIELD_GLOBAL] GlobalManager core: process-global instance, the
// config parse/validate pair, lifecycle (start/stop/tick) and task
// dispatch. The per-domain method groups live in the sibling
// translation units.
// See global_manager.hpp and docs/runtime-global.md.
#include "shield/global/global_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <utility>

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"

namespace shield::global {

namespace {
GlobalManager* g_global_manager = nullptr;
}  // namespace

bool GlobalConfig::from_global_config(GlobalConfig* out, std::string* error) {
    auto& cfg = shield::config::global_config();
    out->cache_max_size =
        static_cast<std::uint64_t>(cfg.get_int("global.cache.max_size", 10000));
    out->cache_default_ttl_ms = static_cast<std::uint64_t>(
        cfg.get_int("global.cache.default_ttl", 60000));
    out->scheduler_tick_ms = static_cast<std::uint64_t>(
        cfg.get_int("global.scheduler.tick_ms", 250));
    out->data_backend = cfg.get_string("global.data_backend", "");
    out->lock_backend = cfg.get_string("global.lock_backend", "");
    out->rank_backend = cfg.get_string("global.rank_backend", "");
    out->redis_host = cfg.get_string("global.redis.host", "");
    out->redis_port = static_cast<int>(cfg.get_int("global.redis.port", 6379));
    out->redis_password = cfg.get_string("global.redis.password", "");
    out->redis_db = static_cast<int>(cfg.get_int("global.redis.db", 0));
    out->redis_prefix = cfg.get_string("global.redis.prefix", "shield:global");
    (void)error;
    return true;
}

bool validate_global_config(const GlobalConfig& config, std::string* error) {
    if (config.cache_max_size == 0) {
        if (error)
            *error =
                "global.cache.max_size must be >= 1";  // GCOVR_EXCL_BR_LINE
                                                       // (null error
                                                       // out-param arm)
        return false;
    }
    if (config.scheduler_tick_ms < 10) {
        if (error)
            *error =
                "global.scheduler.tick_ms must be >= 10";  // GCOVR_EXCL_BR_LINE
                                                           // (null error
                                                           // out-param arm)
        return false;
    }
    if (config.data_backend != "redis" && !config.data_backend.empty()) {
        if (error)
            *error = "global.data_backend must be \"\" or \"redis\" (got \"" +
                     config.data_backend +
                     "\")";  // GCOVR_EXCL_BR_LINE (null error out-param arm)
        return false;
    }
    if (config.data_backend == "redis" && config.redis_host.empty()) {
        if (error)
            *error =
                "global.redis.host is required when global.data_backend "
                "is redis";  // GCOVR_EXCL_BR_LINE (null error out-param
                             // arm)
        return false;
    }
    if (config.lock_backend != "redis" && !config.lock_backend.empty()) {
        if (error)
            *error = "global.lock_backend must be \"\" or \"redis\" (got \"" +
                     config.lock_backend +
                     "\")";  // GCOVR_EXCL_BR_LINE (null error out-param arm)
        return false;
    }
    if (config.lock_backend == "redis" && config.redis_host.empty()) {
        if (error)
            *error =
                "global.redis.host is required when global.lock_backend "
                "is redis";  // GCOVR_EXCL_BR_LINE (null error out-param
                             // arm)
        return false;
    }
    if (config.rank_backend != "redis" && !config.rank_backend.empty()) {
        if (error)
            *error = "global.rank_backend must be \"\" or \"redis\" (got \"" +
                     config.rank_backend +
                     "\")";  // GCOVR_EXCL_BR_LINE (null error out-param arm)
        return false;
    }
    if (config.rank_backend == "redis" && config.redis_host.empty()) {
        if (error)
            *error =
                "global.redis.host is required when global.rank_backend "
                "is redis";  // GCOVR_EXCL_BR_LINE (null error out-param
                             // arm)
        return false;
    }
    return true;
}

GlobalManager::GlobalManager(GlobalConfig config) : config_(std::move(config)) {
    if (config_.data_backend == "redis") {
        RedisDataBackendConfig redis;
        redis.host = config_.redis_host;
        redis.port = config_.redis_port;
        redis.password = config_.redis_password;
        redis.db = config_.redis_db;
        redis.prefix = config_.redis_prefix;
        data_backend_ = make_redis_data_backend(redis);
    } else {
        if (!config_.data_backend.empty()) {
            // Validation rejects unknown values; direct construction falls
            // back to process memory with a warning.
            SHIELD_LOG_WARNING(
                shield::log::get_logger("global"),
                "Unknown global.data_backend: " + config_.data_backend +
                    " (using process memory)");
        }
        data_backend_ = make_inprocess_data_backend();
    }
    if (config_.lock_backend == "redis") {
        RedisDataBackendConfig redis;
        redis.host = config_.redis_host;
        redis.port = config_.redis_port;
        redis.password = config_.redis_password;
        redis.db = config_.redis_db;
        redis.prefix = config_.redis_prefix;
        lock_backend_ = make_redis_lock_backend(redis);
    } else {
        if (!config_.lock_backend.empty()) {
            // Validation rejects unknown values; direct construction falls
            // back to process memory with a warning.
            SHIELD_LOG_WARNING(
                shield::log::get_logger("global"),
                "Unknown global.lock_backend: " + config_.lock_backend +
                    " (using process memory)");
        }
        lock_backend_ = make_inprocess_lock_backend();
    }
    if (config_.rank_backend == "redis") {
        RedisDataBackendConfig redis;
        redis.host = config_.redis_host;
        redis.port = config_.redis_port;
        redis.password = config_.redis_password;
        redis.db = config_.redis_db;
        redis.prefix = config_.redis_prefix;
        rank_backend_ = make_redis_rank_backend(redis);
    } else {
        if (!config_.rank_backend.empty()) {
            // Validation rejects unknown values; direct construction falls
            // back to process memory with a warning.
            SHIELD_LOG_WARNING(
                shield::log::get_logger("global"),
                "Unknown global.rank_backend: " + config_.rank_backend +
                    " (using process memory)");
        }
        rank_backend_ = make_inprocess_rank_backend();
    }
}

GlobalManager::~GlobalManager() { stop(); }

GlobalManager* GlobalManager::global() { return g_global_manager; }

void GlobalManager::set_global(GlobalManager* manager) {
    g_global_manager = manager;
}

std::uint64_t GlobalManager::now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void GlobalManager::set_task_fire_fn(TaskFireFn fn) {
    std::lock_guard<std::mutex> lock(sched_mutex_);
    task_fire_fn_ = std::move(fn);
}

void GlobalManager::start() {
    std::lock_guard<std::mutex> lock(tick_control_mutex_);
    if (tick_running_) {
        return;
    }
    tick_running_ = true;
    tick_thread_ = std::jthread([this](std::stop_token stop) {
        tick_loop();
        (void)stop;
    });
}

void GlobalManager::stop() {
    {
        std::lock_guard<std::mutex> lock(tick_control_mutex_);
        if (!tick_running_) {  // GCOVR_EXCL_BR_LINE (defensive: stop without
                               // start / double stop)
            // Defensive: also drop the callback so a late fire cannot run
            // against a torn-down runtime.
            std::lock_guard<std::mutex> sched_lock(sched_mutex_);
            task_fire_fn_ = nullptr;
            return;
        }
        tick_running_ = false;
    }
    tick_cv_.notify_all();
    if (tick_thread_.joinable()) {  // GCOVR_EXCL_BR_LINE (defensive: thread
                                    // may not be joinable if the scheduler
                                    // never started)
        tick_thread_.request_stop();
        tick_thread_.join();
    }
    std::lock_guard<std::mutex> lock(sched_mutex_);
    task_fire_fn_ = nullptr;
}

// ---- global data ----
//
// Every data_* method delegates to the GlobalDataBackend seam. The
// data_mutex_ guards the delegation AND the local cache, so the manager's
// view of a key cannot interleave with another thread's write (the backend
// takes its own locks inside).

void GlobalManager::fire_task(const SchedTask& task) {
    TaskFireFn fn;
    {
        std::lock_guard<std::mutex> lock(sched_mutex_);
        fn = task_fire_fn_;
    }
    if (!fn) {
        return;
    }
    if (!fn(task.info.service_id, task.info.name)) {
        // Owning service gone: drop the task (server-watch kGone twin).
        std::lock_guard<std::mutex> lock(sched_mutex_);
        tasks_.erase(task.info.name);
    }
}

void GlobalManager::tick_loop() {
    while (true) {
        std::vector<SchedTask> due;
        {
            std::unique_lock<std::mutex> control(tick_control_mutex_);
            if (!tick_running_) {
                return;
            }
            tick_cv_.wait_for(
                control, std::chrono::milliseconds(config_.scheduler_tick_ms),
                [this] { return !tick_running_; });
            if (!tick_running_) {
                return;
            }
        }
        const std::uint64_t now = now_ms();
        {
            std::lock_guard<std::mutex> lock(sched_mutex_);
            for (auto& [name, task] : tasks_) {
                (void)name;
                if (task.info.paused ||
                    task.info.done ||  // GCOVR_EXCL_BR_LINE (defensive:
                                       // pause/done are handled by removal, so
                                       // the tick never sees such tasks)
                    task.info.next_run_ms == 0 ||  // GCOVR_EXCL_BR_LINE
                                                   // (defensive: schedule_set
                                                   // always stores a computed
                                                   // nonzero next_run_ms; the
                                                   // 0 sentinel arm is
                                                   // unreachable for live
                                                   // tasks)
                    task.info.next_run_ms > now) {
                    continue;
                }
                task.info.last_run_ms = now;
                ++task.info.run_count;
                if (task.info.type == "once") {
                    task.info.done = true;
                    task.info.next_run_ms = 0;
                } else if (task.info.type == "interval") {
                    task.info.next_run_ms =
                        now + strtoull(task.info.schedule.c_str(), nullptr, 10);
                } else {
                    task.info.next_run_ms = cron_next(task.cron, now);
                    // Retire arm for schedules whose next match lies
                    // beyond the re-arm horizon: reachable only across
                    // multi-year spans (Feb-29-class gaps), outside any
                    // unit-test wall-clock budget — real behavior, not
                    // defensive dead code.
                    if (task.info.next_run_ms == 0) {  // GCOVR_EXCL_BR_LINE
                        // No match within the horizon: retire the task.
                        task.info.done = true;  // GCOVR_EXCL_LINE
                    }
                }
                due.push_back(task);
            }
        }
        for (const auto& task : due) {
            fire_task(task);
        }
    }
}

}  // namespace shield::global
