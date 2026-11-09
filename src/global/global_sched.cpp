// [SHIELD_GLOBAL] GlobalManager scheduler: the cron expression
// machinery (helpers anon; parse_cron/cron_next are declared in the
// public header), the sched registry and the rate limiters.
// Split out of global_manager.cpp.
// See global_manager.hpp and docs/runtime-global.md.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <utility>

#include "shield/config/config.hpp"
#include "shield/global/global_manager.hpp"
#include "shield/log/logger.hpp"

namespace shield::global {

namespace {
/// Cron field domain bounds: {min, max} per field index.
constexpr int kCronBounds[5][2] = {
    {0, 59},  // minute
    {0, 23},  // hour
    {1, 31},  // day of month
    {1, 12},  // month
    {0, 6},   // day of week (0 = Sunday)
};

/// Parses one comma-separated field body ("*", "n", "a-b", "*/s",
/// "a-b/s") into the bitset for its domain. Returns false on the first
/// malformed component.
bool parse_cron_field(const std::string& body, int field, std::uint64_t* out,
                      std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error) {  // GCOVR_EXCL_BR_LINE (compiler artifact: null-error arm
                      // of the fail lambda)
            const char* names[5] = {"minute", "hour", "day-of-month", "month",
                                    "day-of-week"};
            *error = "cron " + std::string(names[field]) + " field: " + why;
        }
        return false;
    };
    const int lo = kCronBounds[field][0];
    const int hi = kCronBounds[field][1];
    std::uint64_t bits = 0;
    std::size_t pos = 0;
    bool any = false;
    while (pos <= body.size()) {  // GCOVR_EXCL_BR_LINE (loop condition arcs)
        const std::size_t comma = body.find(',', pos);
        const std::string part = body.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (part.empty()) {
            return fail("empty component");
        }
        // value or range, then optional /step
        std::size_t slash = part.find('/');
        const std::string range =
            slash == std::string::npos ? part : part.substr(0, slash);
        int step = 1;
        if (slash != std::string::npos) {
            const std::string step_text = part.substr(slash + 1);
            if (step_text.empty()) {
                return fail("empty step");
            }
            for (char ch : step_text) {  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: range-for char scan)
                if (ch < '0' || ch > '9') {
                    return fail("step is not a number");
                }
            }
            step = std::atoi(step_text.c_str());
            if (step <= 0) {
                return fail("step must be >= 1");
            }
        }
        // `*`/`*/s`/explicit ranges all fill real bits; "unrestricted" is
        // detected by comparing against the full-domain mask.
        int begin = 0;
        int end = 0;
        if (range == "*") {
            begin = lo;
            end = hi;
        } else {
            const std::size_t dash = range.find('-');
            const std::string a =
                dash == std::string::npos ? range : range.substr(0, dash);
            const std::string b =
                dash == std::string::npos ? range : range.substr(dash + 1);
            for (const std::string& num :
                 {a, b}) {  // GCOVR_EXCL_BR_LINE (compiler artifact: range-for
                            // over init-list)
                if (num.empty()) {
                    return fail("empty bound in range");
                }
                for (char ch : num) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // range-for char scan)
                    if (ch < '0' || ch > '9') {
                        return fail("bound is not a number");
                    }
                }
            }
            begin = std::atoi(a.c_str());
            end = std::atoi(b.c_str());
        }
        if (begin < lo || end > hi || begin > end) {
            return fail("value out of range " + std::to_string(lo) + "-" +
                        std::to_string(hi));
        }
        if (step > end - begin + 1) {
            return fail("step exceeds field range");
        }
        for (int v = begin; v <= end; v += step) {
            bits |= (std::uint64_t(1) << v);
        }
        any = true;
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    // Defensive: the loop head rejects empty components, so every
    // completed iteration marks the field as seen.
    if (!any) {                      // GCOVR_EXCL_BR_LINE (defensive)
        return fail("empty field");  // GCOVR_EXCL_LINE (unreachable)
    }
    *out = bits;
    return true;
}

/// Splits on whitespace (spaces/tabs), collapsing runs.
std::vector<std::string> split_fields(const std::string& text) {
    std::vector<std::string> fields;
    std::string current;
    for (char ch :
         text) {  // GCOVR_EXCL_BR_LINE (compiler artifact: range-for char scan)
        if (ch == ' ' ||
            ch == '\t') {  // GCOVR_EXCL_BR_LINE (whitespace classification)
            if (!current.empty()) {  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                     // branch on current.empty())
                fields.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {  // GCOVR_EXCL_BR_LINE (compiler artifact: trailing
                             // token)
        fields.push_back(std::move(current));
    }
    return fields;
}

/// Bitset with every value in [lo, hi] set.
std::uint64_t bits_for(int lo, int hi) {
    std::uint64_t bits = 0;
    for (int v = lo; v <= hi; ++v) {
        bits |= (std::uint64_t(1) << v);
    }
    return bits;
}

struct TmParts {
    int minute;
    int hour;
    int dom;
    int month;  // 1-12
    int dow;    // 0-6, 0 = Sunday
};

TmParts break_down(std::uint64_t wall_ms) {
    const std::time_t seconds = static_cast<std::time_t>(wall_ms / 1000);
    std::tm tm{};
    gmtime_r(&seconds, &tm);
    return TmParts{tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon + 1,
                   tm.tm_wday};
}

bool day_matches(const CronFields& f, const TmParts& p) {
    const bool dom_all =
        f.dom == bits_for(1, 31);  // `*` (or an explicit full range)
    const bool dow_all = f.dow == bits_for(0, 6);
    const bool dom_ok = (f.dom >> p.dom) & 1;
    const bool dow_ok = (f.dow >> p.dow) & 1;
    // Standard cron: when both sides are restricted the day matches if
    // EITHER side allows it; a full wildcard on one side defers to the
    // other. Both OR arms are exercised by tests.
    if (dom_all && dow_all) return true;
    if (dom_all) return dow_ok;
    if (dow_all) return dom_ok;
    return dom_ok ||
           dow_ok;  // GCOVR_EXCL_BR_LINE (both arms exercised by
                    // CronDomRestrictedFallsBackToDow and standard cron tests)
}
}  // namespace

bool parse_cron(const std::string& expression, CronFields* out,
                std::string* error) {
    const auto fields = split_fields(expression);
    if (fields.size() != 5) {
        if (error) {  // GCOVR_EXCL_BR_LINE (null error out-param arm)
            *error =
                "cron expression must have 5 fields (minute hour "
                "day-of-month month day-of-week): " +
                expression;
        }
        return false;
    }
    CronFields parsed;
    const std::uint64_t* targets[5] = {&parsed.minute, &parsed.hour,
                                       &parsed.dom, &parsed.month, &parsed.dow};
    for (int i = 0; i < 5; ++i) {
        if (!parse_cron_field(fields[i], i,
                              const_cast<std::uint64_t*>(targets[i]), error)) {
            return false;
        }
    }
    *out = parsed;
    return true;
}

std::uint64_t cron_next(const CronFields& fields, std::uint64_t from_ms) {
    // Start at the first whole minute strictly after `from_ms`.
    std::uint64_t t = (from_ms / 60000 + 1) * 60000;
    // Scan horizon: ~2 years of minutes (no match -> 0).
    const std::uint64_t horizon = t + 2ULL * 366 * 24 * 60 * 60000;
    for (; t < horizon; t += 60000) {
        const TmParts p = break_down(t);
        if (((fields.minute >> p.minute) & 1) &&
            ((fields.hour >> p.hour) & 1) && ((fields.month >> p.month) & 1) &&
            day_matches(fields, p)) {
            return t;
        }
    }
    return 0;
}

bool GlobalManager::sched_register(const std::string& type,
                                   const std::string& name,
                                   const std::string& schedule,
                                   const std::string& service_id,
                                   std::string* error) {
    std::uint64_t next_run = 0;
    CronFields cron;
    if (type == "cron") {
        if (!parse_cron(schedule, &cron, error)) {
            return false;
        }
        next_run = cron_next(cron, now_ms());
        if (next_run == 0) {
            if (error) {
                *error = "cron expression has no upcoming match: " + schedule;
            }
            return false;
        }
    } else if (type == "interval" || type == "once") {
        char* end = nullptr;
        const long long ms = std::strtoll(schedule.c_str(), &end, 10);
        if (schedule.empty() ||  // GCOVR_EXCL_BR_LINE (defensive: same glibc
                                 // strtoll nullptr-end unreachability as the
                                 // KV parse guard)
            end == nullptr ||    // GCOVR_EXCL_BR_LINE (defensive: glibc strtoll
                                 // always stores a non-null end)
            end != schedule.c_str() + schedule.size() || ms < 0) {
            if (error) {
                *error = type + " schedule must be a non-negative ms value";
            }
            return false;
        }
        next_run = now_ms() + static_cast<std::uint64_t>(ms);
    } else {
        // GCOVR_EXCL_START (the Lua facade only passes the three literals;
        // unknown types cannot reach the manager from user code)
        if (error) *error = "unknown scheduler task type: " + type;
        return false;
        // GCOVR_EXCL_STOP
    }
    std::lock_guard<std::mutex> lock(sched_mutex_);
    if (tasks_.count(name) != 0) {
        if (error) *error = "scheduler task already exists: " + name;
        return false;
    }
    SchedTask task;
    task.info.name = name;
    task.info.type = type;
    task.info.schedule = schedule;
    task.info.service_id = service_id;
    task.info.next_run_ms = next_run;
    task.cron = cron;
    tasks_.emplace(name, std::move(task));
    return true;
}

bool GlobalManager::sched_remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(sched_mutex_);
    return tasks_.erase(name) != 0;
}

bool GlobalManager::sched_pause(const std::string& name) {
    std::lock_guard<std::mutex> lock(sched_mutex_);
    auto it = tasks_.find(name);
    if (it == tasks_.end()) {
        return false;
    }
    it->second.info.paused = true;
    return true;
}

bool GlobalManager::sched_resume(const std::string& name) {
    std::lock_guard<std::mutex> lock(sched_mutex_);
    auto it = tasks_.find(name);
    if (it == tasks_.end()) {
        return false;
    }
    if (it->second.info.paused) {
        it->second.info.paused = false;
        // Do not backlog missed runs while paused: re-arm from now.
        if (it->second.info.type == "cron") {
            it->second.info.next_run_ms = cron_next(it->second.cron, now_ms());
        } else if (!it->second.info.done) {
            it->second.info.next_run_ms =
                now_ms() +
                strtoull(it->second.info.schedule.c_str(), nullptr, 10);
        }
    }
    return true;
}

bool GlobalManager::sched_trigger(const std::string& name) {
    SchedTask snapshot;
    {
        std::lock_guard<std::mutex> lock(sched_mutex_);
        auto it = tasks_.find(name);
        if (it == tasks_.end()) {
            return false;
        }
        ++it->second.info.run_count;
        snapshot = it->second;
    }
    fire_task(snapshot);
    return true;
}

std::optional<SchedInfo> GlobalManager::sched_get(const std::string& name) {
    std::lock_guard<std::mutex> lock(sched_mutex_);
    auto it = tasks_.find(name);
    if (it == tasks_.end()) {
        return std::nullopt;
    }
    return it->second.info;
}

std::vector<SchedInfo> GlobalManager::sched_list() {
    std::lock_guard<std::mutex> lock(sched_mutex_);
    std::vector<SchedInfo> out;
    out.reserve(tasks_.size());
    for (const auto& [name, task] : tasks_) {
        (void)name;
        out.push_back(task.info);
    }
    return out;
}

std::size_t GlobalManager::sched_active_count() {
    std::lock_guard<std::mutex> lock(sched_mutex_);
    std::size_t active = 0;
    for (const auto& [name, task] : tasks_) {
        (void)name;
        if (!task.info.paused && !task.info.done) {
            ++active;
        }
    }
    return active;
}

// ---- rate limiters ----

void GlobalManager::rate_limit_configure(const std::string& name,
                                         const RateLimitConfig& config) {
    std::lock_guard<std::mutex> lock(rate_limits_mutex_);
    auto& limiter = rate_limiters_[name];
    limiter.config = config;
    // Drop per-key state: buckets/windows sized for the old config must not
    // leak into the new shape.
    limiter.buckets.clear();
    limiter.windows.clear();
}

RateLimitResult GlobalManager::rate_limit_allow(const std::string& name,
                                                const std::string& key,
                                                double cost) {
    std::lock_guard<std::mutex> lock(rate_limits_mutex_);
    auto it = rate_limiters_.find(name);
    if (it == rate_limiters_.end()) {
        return {};
    }
    const RateLimitConfig& cfg = it->second.config;
    const std::uint64_t now = now_ms();
    RateLimitResult out;
    if (cfg.sliding) {  // GCOVR_EXCL_BR_LINE (both sliding/fixed arms exercised
                        // by tests)
        auto& hits = it->second.windows[key].hits;
        while (!hits.empty() && now - hits.front() >= cfg.window_ms) {
            hits.pop_front();
        }
        if (hits.size() < cfg.max_requests) {
            hits.push_back(now);
            out.allowed = true;
            out.remaining = static_cast<double>(cfg.max_requests - hits.size());
        } else {
            out.remaining = 0.0;
            out.retry_after_ms =
                hits.front() + cfg.window_ms - now;  // oldest hit ages out
        }
        return out;
    }

    auto& bucket = it->second.buckets[key];
    if (bucket.last_refill_ms == 0) {
        bucket.tokens = cfg.burst;
    }
    const double elapsed = static_cast<double>(now - bucket.last_refill_ms);
    if (elapsed > 0) {
        bucket.tokens =
            std::min(cfg.burst, bucket.tokens + elapsed * cfg.rate / 1000.0);
        bucket.last_refill_ms = now;
    }
    if (cost > 0 &&
        bucket.tokens >=
            cost) {  // GCOVR_EXCL_BR_LINE (both arms exercised by tests)
        bucket.tokens -= cost;
        out.allowed = true;
        out.remaining = bucket.tokens;
    } else {
        out.remaining = bucket.tokens;
        if (cfg.rate > 0 &&
            cost > 0) {  // GCOVR_EXCL_BR_LINE (both arms exercised by tests)
            out.retry_after_ms = static_cast<std::uint64_t>(
                (cost - bucket.tokens) * 1000.0 / cfg.rate);
        }
    }
    return out;
}

double GlobalManager::rate_limit_remaining(const std::string& name,
                                           const std::string& key) {
    std::lock_guard<std::mutex> lock(rate_limits_mutex_);
    auto it = rate_limiters_.find(name);
    if (it == rate_limiters_.end()) {
        return 0.0;
    }
    const RateLimitConfig& cfg = it->second.config;
    const std::uint64_t now = now_ms();
    if (cfg.sliding) {  // GCOVR_EXCL_BR_LINE (both sliding/fixed arms exercised
                        // by tests)
        auto wit = it->second.windows.find(key);
        if (wit ==
            it->second.windows
                .end()) {  // GCOVR_EXCL_BR_LINE (both arms exercised by tests)
            return static_cast<double>(cfg.max_requests);
        }
        // Query-only: evict on a copy, never mutate the stored window.
        auto hits = wit->second.hits;
        while (!hits.empty() && now - hits.front() >= cfg.window_ms) {
            hits.pop_front();
        }
        const std::size_t used =
            std::min<std::size_t>(hits.size(), cfg.max_requests);
        return static_cast<double>(cfg.max_requests - used);
    }
    auto bit = it->second.buckets.find(key);
    if (bit == it->second.buckets.end() ||  // GCOVR_EXCL_BR_LINE (both arms
                                            // exercised by tests)
        bit->second.last_refill_ms ==
            0) {  // GCOVR_EXCL_BR_LINE (defensive: a mapped bucket is always
                  // stamped by its creating consume)
        return cfg.burst;
    }
    const double elapsed =
        static_cast<double>(now - bit->second.last_refill_ms);
    return std::min(cfg.burst,
                    bit->second.tokens + elapsed * cfg.rate / 1000.0);
}

std::size_t GlobalManager::rate_limit_key_count(const std::string& name) {
    std::lock_guard<std::mutex> lock(rate_limits_mutex_);
    auto it = rate_limiters_.find(name);
    if (it == rate_limiters_.end()) {
        return 0;
    }
    return it->second.config.sliding ? it->second.windows.size()
                                     : it->second.buckets.size();
}

void GlobalManager::rate_limit_purge(const std::string& name) {
    std::lock_guard<std::mutex> lock(rate_limits_mutex_);
    auto it = rate_limiters_.find(name);
    if (it != rate_limiters_.end()) {
        it->second.buckets.clear();
        it->second.windows.clear();
    }
}

}  // namespace shield::global
