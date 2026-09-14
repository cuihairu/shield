// [SHIELD_GLOBAL] Process-wide global-capability store (P0).
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
        if (error) {
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
    while (pos <= body.size()) {
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
            for (char ch : step_text) {
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
            for (const std::string& num : {a, b}) {
                if (num.empty()) {
                    return fail("empty bound in range");
                }
                for (char ch : num) {
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
        for (int v = begin; v <= end; v += step) {
            bits |= (std::uint64_t(1) << v);
        }
        any = true;
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    if (!any) {
        return fail("empty field");
    }
    *out = bits;
    return true;
}

/// Splits on whitespace (spaces/tabs), collapsing runs.
std::vector<std::string> split_fields(const std::string& text) {
    std::vector<std::string> fields;
    std::string current;
    for (char ch : text) {
        if (ch == ' ' || ch == '\t') {
            if (!current.empty()) {
                fields.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
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
    // other.
    if (dom_all && dow_all) return true;
    if (dom_all) return dow_ok;
    if (dow_all) return dom_ok;
    return dom_ok || dow_ok;
}

}  // namespace

bool parse_cron(const std::string& expression, CronFields* out,
                std::string* error) {
    const auto fields = split_fields(expression);
    if (fields.size() != 5) {
        if (error) {
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

bool GlobalConfig::from_global_config(GlobalConfig* out, std::string* error) {
    auto& cfg = shield::config::global_config();
    out->cache_max_size =
        static_cast<std::uint64_t>(cfg.get_int("global.cache.max_size", 10000));
    out->cache_default_ttl_ms = static_cast<std::uint64_t>(
        cfg.get_int("global.cache.default_ttl", 60000));
    out->scheduler_tick_ms = static_cast<std::uint64_t>(
        cfg.get_int("global.scheduler.tick_ms", 250));
    (void)error;
    return true;
}

bool validate_global_config(const GlobalConfig& config, std::string* error) {
    if (config.cache_max_size == 0) {
        if (error) *error = "global.cache.max_size must be >= 1";
        return false;
    }
    if (config.scheduler_tick_ms < 10) {
        if (error) *error = "global.scheduler.tick_ms must be >= 10";
        return false;
    }
    return true;
}

GlobalManager::GlobalManager(GlobalConfig config)
    : config_(std::move(config)) {}

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
        if (!tick_running_) {
            // Defensive: also drop the callback so a late fire cannot run
            // against a torn-down runtime.
            std::lock_guard<std::mutex> sched_lock(sched_mutex_);
            task_fire_fn_ = nullptr;
            return;
        }
        tick_running_ = false;
    }
    tick_cv_.notify_all();
    if (tick_thread_.joinable()) {
        tick_thread_.request_stop();
        tick_thread_.join();
    }
    std::lock_guard<std::mutex> lock(sched_mutex_);
    task_fire_fn_ = nullptr;
}

// ---- global data ----

bool GlobalManager::data_get(const std::string& key, std::string* out) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    auto it = data_.find(key);
    if (it == data_.end()) {
        return false;
    }
    auto exp = data_expire_.find(key);
    if (exp != data_expire_.end() && exp->second <= now_ms()) {
        data_.erase(it);
        data_expire_.erase(exp);
        // A lazy data expiry must also drop the cached copy.
        auto idx = cache_index_.find(key);
        if (idx != cache_index_.end()) {
            cache_lru_.erase(idx->second);
            cache_index_.erase(idx);
            cache_expire_.erase(key);
        }
        return false;
    }
    if (out) {
        *out = it->second;
    }
    return true;
}

void GlobalManager::data_set(const std::string& key, std::string value,
                             std::uint64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    data_[key] = std::move(value);
    if (ttl_ms != 0) {
        data_expire_[key] = now_ms() + ttl_ms;
    } else {
        data_expire_.erase(key);
    }
    // Writes invalidate the cached copy (cache coherence contract).
    auto idx = cache_index_.find(key);
    if (idx != cache_index_.end()) {
        cache_lru_.erase(idx->second);
        cache_index_.erase(idx);
        cache_expire_.erase(key);
    }
}

bool GlobalManager::data_delete(const std::string& key) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    const bool erased = data_.erase(key) != 0;
    data_expire_.erase(key);
    auto idx = cache_index_.find(key);
    if (idx != cache_index_.end()) {
        cache_lru_.erase(idx->second);
        cache_index_.erase(idx);
        cache_expire_.erase(key);
    }
    return erased;
}

bool GlobalManager::data_incr_by(const std::string& key, std::int64_t delta,
                                 std::int64_t* out, std::string* error) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    auto exp = data_expire_.find(key);
    if (exp != data_expire_.end() && exp->second <= now_ms()) {
        data_.erase(key);
        data_expire_.erase(exp);
    }
    auto it = data_.find(key);
    std::int64_t current = 0;
    if (it != data_.end()) {
        const std::string& text = it->second;
        char* end = nullptr;
        const long long parsed = std::strtoll(text.c_str(), &end, 10);
        if (text.empty() || end == nullptr ||
            end != text.c_str() + text.size()) {
            if (error) {
                *error = "value of '" + key + "' is not an integer";
            }
            return false;
        }
        current = static_cast<std::int64_t>(parsed);
    }
    current += delta;
    data_[key] = std::to_string(current);
    if (out) {
        *out = current;
    }
    return true;
}

std::vector<std::optional<std::string>> GlobalManager::data_mget(
    const std::vector<std::string>& keys) {
    std::vector<std::optional<std::string>> values;
    values.reserve(keys.size());
    for (const auto& key : keys) {
        std::string value;
        if (data_get(key, &value)) {
            values.emplace_back(std::move(value));
        } else {
            values.emplace_back();
        }
    }
    return values;
}

bool GlobalManager::data_mset(
    const std::vector<std::pair<std::string, std::string>>& kvs,
    std::uint64_t ttl_ms, std::string* error) {
    for (const auto& [key, value] : kvs) {
        if (key.empty()) {
            if (error) *error = "mset key must not be empty";
            return false;
        }
        (void)value;
    }
    for (const auto& [key, value] : kvs) {
        data_set(key, value, ttl_ms);
    }
    return true;
}

std::size_t GlobalManager::data_size() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return data_.size();
}

// ---- local cache ----

bool GlobalManager::cache_get(const std::string& key, std::uint64_t ttl_ms,
                              std::string* out) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    const std::uint64_t now = now_ms();
    auto idx = cache_index_.find(key);
    if (idx != cache_index_.end()) {
        auto exp = cache_expire_.find(key);
        if (exp != cache_expire_.end() && exp->second <= now) {
            cache_lru_.erase(idx->second);
            cache_index_.erase(idx);
            cache_expire_.erase(exp);
        } else {
            // Hit: refresh LRU position and hand back the cached copy.
            cache_lru_.splice(cache_lru_.begin(), cache_lru_, idx->second);
            ++cache_hits_;
            if (out) {
                *out = idx->second->second;
            }
            return true;
        }
    }
    ++cache_misses_;
    // Miss: fill from the data domain (respecting its TTL too).
    auto it = data_.find(key);
    auto data_exp = data_expire_.find(key);
    if (it == data_.end() ||
        (data_exp != data_expire_.end() && data_exp->second <= now)) {
        if (it != data_.end()) {
            data_.erase(it);
            data_expire_.erase(data_exp);
        }
        return false;
    }
    const std::uint64_t effective_ttl =
        ttl_ms != 0 ? ttl_ms : config_.cache_default_ttl_ms;
    cache_lru_.emplace_front(key, it->second);
    cache_index_[key] = cache_lru_.begin();
    if (effective_ttl != 0) {
        cache_expire_[key] = now + effective_ttl;
    } else {
        cache_expire_.erase(key);
    }
    while (cache_index_.size() > config_.cache_max_size) {
        const std::string& victim = cache_lru_.back().first;
        cache_index_.erase(victim);
        cache_expire_.erase(victim);
        cache_lru_.pop_back();
    }
    if (out) {
        *out = it->second;
    }
    return true;
}

void GlobalManager::cache_invalidate(const std::string& key) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    auto idx = cache_index_.find(key);
    if (idx != cache_index_.end()) {
        cache_lru_.erase(idx->second);
        cache_index_.erase(idx);
        cache_expire_.erase(key);
    }
}

void GlobalManager::cache_clear() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    cache_lru_.clear();
    cache_index_.clear();
    cache_expire_.clear();
}

std::size_t GlobalManager::cache_size() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return cache_index_.size();
}

std::uint64_t GlobalManager::cache_hits() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return cache_hits_;
}

std::uint64_t GlobalManager::cache_misses() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return cache_misses_;
}

// ---- exclusive locks ----

bool GlobalManager::mutex_live(const MutexEntry& entry, std::uint64_t now) {
    return entry.expire_at_ms == 0 || now < entry.expire_at_ms;
}

bool GlobalManager::mutex_try_acquire_locked(
    std::unordered_map<std::string, MutexEntry>& registry,
    const std::string& name, const std::string& owner, std::uint64_t ttl_ms) {
    const std::uint64_t now = now_ms();
    auto it = registry.find(name);
    if (it == registry.end()) {
        MutexEntry entry;
        entry.owner = owner;
        entry.count = 1;
        entry.acquired_at_ms = now;
        entry.expire_at_ms = ttl_ms != 0 ? now + ttl_ms : 0;
        registry.emplace(name, std::move(entry));
        return true;
    }
    MutexEntry& entry = it->second;
    if (!mutex_live(entry, now)) {
        // Lazy TTL expiry: the stale holder lost the lock.
        entry.owner = owner;
        entry.count = 1;
        entry.acquired_at_ms = now;
        entry.expire_at_ms = ttl_ms != 0 ? now + ttl_ms : 0;
        return true;
    }
    if (entry.owner == owner) {
        ++entry.count;
        if (ttl_ms != 0) {
            entry.expire_at_ms = now + ttl_ms;
        }
        return true;
    }
    return false;
}

LockStatus GlobalManager::mutex_acquire(const std::string& registry,
                                        const std::string& name,
                                        const std::string& owner,
                                        std::uint64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    auto& table = registry == "spinlock" ? spinlocks_ : mutexes_;
    return mutex_try_acquire_locked(table, name, owner, ttl_ms)
               ? LockStatus::kOk
               : LockStatus::kBusy;
}

LockStatus GlobalManager::mutex_release(const std::string& registry,
                                        const std::string& name,
                                        const std::string& owner) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    auto& table = registry == "spinlock" ? spinlocks_ : mutexes_;
    auto it = table.find(name);
    if (it == table.end() || it->second.owner != owner) {
        return LockStatus::kNotOwner;
    }
    if (--it->second.count == 0) {
        table.erase(it);
    }
    return LockStatus::kOk;
}

bool GlobalManager::mutex_extend(const std::string& registry,
                                 const std::string& name,
                                 const std::string& owner,
                                 std::uint64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    auto& table = registry == "spinlock" ? spinlocks_ : mutexes_;
    auto it = table.find(name);
    if (it == table.end() || it->second.owner != owner ||
        !mutex_live(it->second, now_ms())) {
        return false;
    }
    it->second.expire_at_ms = ttl_ms != 0 ? now_ms() + ttl_ms : 0;
    return true;
}

LockInfo GlobalManager::mutex_info(const std::string& registry,
                                   const std::string& name) const {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    const auto& table = registry == "spinlock" ? spinlocks_ : mutexes_;
    auto it = table.find(name);
    if (it == table.end()) {
        return LockInfo{};
    }
    LockInfo info;
    info.exists = true;
    info.owner = it->second.owner;
    info.count = it->second.count;
    info.acquired_at_ms = it->second.acquired_at_ms;
    if (it->second.expire_at_ms != 0) {
        const std::uint64_t now = now_ms();
        info.ttl_remaining_ms =
            now < it->second.expire_at_ms ? it->second.expire_at_ms - now : 0;
    }
    return info;
}

std::size_t GlobalManager::mutex_registry_size(
    const std::string& registry) const {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    const auto& table = registry == "spinlock" ? spinlocks_ : mutexes_;
    return table.size();
}

// ---- rwlocks ----

LockStatus GlobalManager::rw_write_acquire(const std::string& name,
                                           const std::string& owner,
                                           std::uint64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    const std::uint64_t now = now_ms();
    auto it = rwlocks_.find(name);
    if (it == rwlocks_.end()) {
        RwLockEntry entry;
        entry.write_owner = owner;
        entry.write_count = 1;
        entry.write_expire_at_ms = ttl_ms != 0 ? now + ttl_ms : 0;
        rwlocks_.emplace(name, std::move(entry));
        return LockStatus::kOk;
    }
    RwLockEntry& entry = it->second;
    const bool write_live =
        entry.write_expire_at_ms == 0 || now < entry.write_expire_at_ms;
    if (!write_live) {
        // Stale writer: the lock body resets.
        entry.write_owner = owner;
        entry.write_count = 1;
        entry.write_expire_at_ms = ttl_ms != 0 ? now + ttl_ms : 0;
        entry.readers.clear();
        return LockStatus::kOk;
    }
    if (!entry.write_owner.empty()) {
        if (entry.write_owner == owner) {
            ++entry.write_count;
            if (ttl_ms != 0) {
                entry.write_expire_at_ms = now + ttl_ms;
            }
            return LockStatus::kOk;
        }
        return LockStatus::kBusy;
    }
    if (!entry.readers.empty()) {
        return LockStatus::kBusy;
    }
    entry.write_owner = owner;
    entry.write_count = 1;
    entry.write_expire_at_ms = ttl_ms != 0 ? now + ttl_ms : 0;
    return LockStatus::kOk;
}

LockStatus GlobalManager::rw_write_release(const std::string& name,
                                           const std::string& owner) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    auto it = rwlocks_.find(name);
    if (it == rwlocks_.end() || it->second.write_owner != owner) {
        return LockStatus::kNotOwner;
    }
    if (--it->second.write_count == 0) {
        it->second.write_owner.clear();
        it->second.write_expire_at_ms = 0;
    }
    return LockStatus::kOk;
}

bool GlobalManager::rw_write_extend(const std::string& name,
                                    const std::string& owner,
                                    std::uint64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    auto it = rwlocks_.find(name);
    if (it == rwlocks_.end() || it->second.write_owner != owner) {
        return false;
    }
    if (it->second.write_expire_at_ms != 0 &&
        now_ms() >= it->second.write_expire_at_ms) {
        return false;
    }
    it->second.write_expire_at_ms = ttl_ms != 0 ? now_ms() + ttl_ms : 0;
    return true;
}

LockStatus GlobalManager::rw_read_acquire(const std::string& name,
                                          const std::string& owner) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    const std::uint64_t now = now_ms();
    auto it = rwlocks_.find(name);
    if (it == rwlocks_.end()) {
        RwLockEntry entry;
        entry.readers[owner] = 1;
        rwlocks_.emplace(name, std::move(entry));
        return LockStatus::kOk;
    }
    RwLockEntry& entry = it->second;
    const bool write_live =
        entry.write_expire_at_ms == 0 || now < entry.write_expire_at_ms;
    if (!write_live) {
        entry.write_owner.clear();
        entry.write_count = 0;
        entry.write_expire_at_ms = 0;
        entry.readers.clear();
    }
    if (!entry.write_owner.empty()) {
        return LockStatus::kBusy;
    }
    ++entry.readers[owner];
    return LockStatus::kOk;
}

LockStatus GlobalManager::rw_read_release(const std::string& name,
                                          const std::string& owner) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    auto it = rwlocks_.find(name);
    if (it == rwlocks_.end()) {
        return LockStatus::kNotOwner;
    }
    auto reader = it->second.readers.find(owner);
    if (reader == it->second.readers.end()) {
        return LockStatus::kNotOwner;
    }
    if (--reader->second == 0) {
        it->second.readers.erase(reader);
    }
    return LockStatus::kOk;
}

RwLockInfo GlobalManager::rwlock_info(const std::string& name) const {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    auto it = rwlocks_.find(name);
    if (it == rwlocks_.end()) {
        return RwLockInfo{};
    }
    RwLockInfo info;
    info.exists = true;
    info.write_owner = it->second.write_owner;
    info.write_count = it->second.write_count;
    if (it->second.write_expire_at_ms != 0) {
        const std::uint64_t now = now_ms();
        info.write_ttl_remaining_ms = now < it->second.write_expire_at_ms
                                          ? it->second.write_expire_at_ms - now
                                          : 0;
    }
    info.readers = it->second.readers.size();
    return info;
}

std::size_t GlobalManager::rwlock_count() const {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    return rwlocks_.size();
}

// ---- leaderboards ----

std::vector<RankEntry> GlobalManager::rank_sorted_locked(
    const std::unordered_map<std::string, double>& board) {
    std::vector<RankEntry> entries;
    entries.reserve(board.size());
    for (const auto& [uid, score] : board) {
        entries.push_back(RankEntry{uid, score, 0});
    }
    std::sort(entries.begin(), entries.end(),
              [](const RankEntry& a, const RankEntry& b) {
                  if (a.score != b.score) {
                      return a.score > b.score;
                  }
                  return a.uid < b.uid;
              });
    for (std::size_t i = 0; i < entries.size(); ++i) {
        entries[i].rank = i + 1;
    }
    return entries;
}

void GlobalManager::rank_update(const std::string& board,
                                const std::string& uid, double score) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    ranks_[board][uid] = score;
}

void GlobalManager::rank_mupdate(
    const std::string& board,
    const std::vector<std::pair<std::string, double>>& updates) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    auto& target = ranks_[board];
    for (const auto& [uid, score] : updates) {
        target[uid] = score;
    }
}

std::optional<double> GlobalManager::rank_score(const std::string& board,
                                                const std::string& uid) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    auto it = ranks_.find(board);
    if (it == ranks_.end()) {
        return std::nullopt;
    }
    auto entry = it->second.find(uid);
    if (entry == it->second.end()) {
        return std::nullopt;
    }
    return entry->second;
}

std::optional<std::uint64_t> GlobalManager::rank_position(
    const std::string& board, const std::string& uid) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    auto it = ranks_.find(board);
    if (it == ranks_.end()) {
        return std::nullopt;
    }
    const auto sorted = rank_sorted_locked(it->second);
    for (const auto& entry : sorted) {
        if (entry.uid == uid) {
            return entry.rank;
        }
    }
    return std::nullopt;
}

std::vector<RankEntry> GlobalManager::rank_top(const std::string& board,
                                               std::size_t n) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    auto it = ranks_.find(board);
    if (it == ranks_.end() || n == 0) {
        return {};
    }
    auto sorted = rank_sorted_locked(it->second);
    if (sorted.size() > n) {
        sorted.resize(n);
    }
    return sorted;
}

std::vector<RankEntry> GlobalManager::rank_range(const std::string& board,
                                                 std::uint64_t from,
                                                 std::uint64_t to) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    auto it = ranks_.find(board);
    if (it == ranks_.end() || from == 0 || to < from) {
        return {};
    }
    const auto sorted = rank_sorted_locked(it->second);
    const std::size_t begin = std::min<std::size_t>(
        static_cast<std::size_t>(from - 1), sorted.size());
    const std::size_t end =
        std::min<std::size_t>(static_cast<std::size_t>(to), sorted.size());
    return std::vector<RankEntry>(sorted.begin() + begin, sorted.begin() + end);
}

std::vector<RankEntry> GlobalManager::rank_range_by_score(
    const std::string& board, double lo, double hi) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    auto it = ranks_.find(board);
    if (it == ranks_.end() || hi < lo) {
        return {};
    }
    std::vector<RankEntry> out;
    for (const auto& entry : rank_sorted_locked(it->second)) {
        if (entry.score >= lo && entry.score <= hi) {
            out.push_back(entry);
        }
    }
    return out;
}

RankAround GlobalManager::rank_around(const std::string& board,
                                      const std::string& uid, std::size_t n) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    RankAround around;
    auto it = ranks_.find(board);
    if (it == ranks_.end()) {
        return around;
    }
    const auto sorted = rank_sorted_locked(it->second);
    std::size_t pos = sorted.size();
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (sorted[i].uid == uid) {
            pos = i;
            break;
        }
    }
    if (pos == sorted.size()) {
        return around;
    }
    around.target = sorted[pos];
    const std::size_t first = pos > n ? pos - n : 0;
    around.above.assign(sorted.begin() + first, sorted.begin() + pos);
    around.below.assign(sorted.begin() + pos + 1,
                        sorted.begin() + std::min(pos + 1 + n, sorted.size()));
    return around;
}

std::size_t GlobalManager::rank_count(const std::string& board) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    auto it = ranks_.find(board);
    return it == ranks_.end() ? 0 : it->second.size();
}

bool GlobalManager::rank_remove(const std::string& board,
                                const std::string& uid) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    auto it = ranks_.find(board);
    if (it == ranks_.end()) {
        return false;
    }
    return it->second.erase(uid) != 0;
}

void GlobalManager::rank_clear(const std::string& board) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    auto it = ranks_.find(board);
    if (it != ranks_.end()) {
        it->second.clear();
    }
}

std::size_t GlobalManager::rank_board_count() {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    std::size_t non_empty = 0;
    for (const auto& [board, entries] : ranks_) {
        (void)board;
        if (!entries.empty()) {
            ++non_empty;
        }
    }
    return non_empty;
}

std::size_t GlobalManager::rank_total_members() {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    std::size_t total = 0;
    for (const auto& [board, entries] : ranks_) {
        (void)board;
        total += entries.size();
    }
    return total;
}

// ---- normal queues ----

void GlobalManager::queue_push(const std::string& name, std::string payload) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    queues_[name].push_back(std::move(payload));
}

void GlobalManager::queue_push_batch(const std::string& name,
                                     const std::vector<std::string>& payloads) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto& queue = queues_[name];
    for (const auto& payload : payloads) {
        queue.push_back(payload);
    }
}

bool GlobalManager::queue_pop(const std::string& name, std::string* out) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = queues_.find(name);
    if (it == queues_.end() || it->second.empty()) {
        return false;
    }
    if (out) {
        *out = std::move(it->second.front());
    }
    it->second.pop_front();
    return true;
}

std::size_t GlobalManager::queue_length(const std::string& name) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = queues_.find(name);
    return it == queues_.end() ? 0 : it->second.size();
}

void GlobalManager::queue_purge(const std::string& name) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    queues_.erase(name);
}

std::size_t GlobalManager::queue_count() {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    return queues_.size();
}

// ---- delay queues ----

void GlobalManager::delay_push(const std::string& name, std::string payload,
                               std::uint64_t delay_ms) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    delay_queues_[name].emplace(now_ms() + delay_ms,
                                DelayedItem{0, std::move(payload)});
}

void GlobalManager::delay_push_at(const std::string& name, std::string payload,
                                  std::uint64_t at_ms) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    delay_queues_[name].emplace(at_ms, DelayedItem{0, std::move(payload)});
}

bool GlobalManager::delay_pop(const std::string& name, std::string* out) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = delay_queues_.find(name);
    if (it == delay_queues_.end() || it->second.empty() ||
        it->second.begin()->first > now_ms()) {
        return false;
    }
    if (out) {
        *out = std::move(it->second.begin()->second.payload);
    }
    it->second.erase(it->second.begin());
    return true;
}

std::size_t GlobalManager::delay_pending(const std::string& name) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = delay_queues_.find(name);
    if (it == delay_queues_.end()) {
        return 0;
    }
    const std::uint64_t now = now_ms();
    std::size_t pending = 0;
    for (const auto& [ready_at, item] : it->second) {
        (void)item;
        if (ready_at > now) {
            ++pending;
        }
    }
    return pending;
}

std::size_t GlobalManager::delay_ready(const std::string& name) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = delay_queues_.find(name);
    if (it == delay_queues_.end()) {
        return 0;
    }
    const std::uint64_t now = now_ms();
    std::size_t ready = 0;
    for (const auto& [ready_at, item] : it->second) {
        (void)item;
        if (ready_at <= now) {
            ++ready;
        }
    }
    return ready;
}

void GlobalManager::delay_purge(const std::string& name) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    delay_queues_.erase(name);
}

std::size_t GlobalManager::delay_queue_count() {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    return delay_queues_.size();
}

// ---- reliable queues ----

void GlobalManager::reliable_configure(const std::string& name,
                                       int max_retries) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    if (max_retries > 0) {
        reliable_queues_[name].max_retries = max_retries;
    }
}

void GlobalManager::reliable_push(const std::string& name,
                                  std::string payload) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto& queue = reliable_queues_[name];
    queue.pending.emplace(
        now_ms(), DelayedItem{queue.next_delivery_id++, std::move(payload)});
}
bool GlobalManager::reliable_pop(const std::string& name,
                                 ReliableDelivery* out) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = reliable_queues_.find(name);
    if (it == reliable_queues_.end() || it->second.pending.empty() ||
        it->second.pending.begin()->first > now_ms()) {
        return false;
    }
    auto& queue = it->second;
    auto first = queue.pending.begin();
    const std::uint64_t retries =
        queue.retry_counts.count(first->second.id) != 0
            ? queue.retry_counts[first->second.id]
            : 0;
    DeadItem item{first->second.id, std::move(first->second.payload), retries};
    queue.pending.erase(first);
    const std::uint64_t delivery_id = item.delivery_id;
    queue.inflight.emplace(delivery_id, std::move(item));
    if (out) {
        out->delivery_id = delivery_id;
        out->payload = queue.inflight[delivery_id].payload;
    }
    return true;
}

bool GlobalManager::reliable_ack(const std::string& name,
                                 std::uint64_t delivery_id) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = reliable_queues_.find(name);
    if (it == reliable_queues_.end()) {
        return false;
    }
    if (it->second.inflight.erase(delivery_id) == 0) {
        return false;
    }
    it->second.retry_counts.erase(delivery_id);
    return true;
}

NackResult GlobalManager::reliable_nack(const std::string& name,
                                        std::uint64_t delivery_id,
                                        std::uint64_t retry_ms) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = reliable_queues_.find(name);
    if (it == reliable_queues_.end()) {
        return NackResult::kNotFound;
    }
    auto& queue = it->second;
    auto inflight = queue.inflight.find(delivery_id);
    if (inflight == queue.inflight.end()) {
        return NackResult::kNotFound;
    }
    DeadItem item = std::move(inflight->second);
    queue.inflight.erase(inflight);
    item.retries += 1;
    if (item.retries >= static_cast<std::uint64_t>(queue.max_retries)) {
        // Retries exhausted: dead letter, retry bookkeeping dropped.
        queue.retry_counts.erase(item.delivery_id);
        queue.dead.push_back(std::move(item));
        return NackResult::kDead;
    }
    queue.retry_counts[item.delivery_id] = item.retries;
    queue.pending.emplace(now_ms() + retry_ms,
                          DelayedItem{item.delivery_id, item.payload});
    return NackResult::kRequeued;
}

std::vector<DeadEntry> GlobalManager::reliable_dead_range(
    const std::string& name, std::size_t from, std::size_t to) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = reliable_queues_.find(name);
    if (it == reliable_queues_.end() || from >= it->second.dead.size()) {
        return {};
    }
    const auto& dead = it->second.dead;
    const std::size_t end = std::min<std::size_t>(to + 1, dead.size());
    std::vector<DeadEntry> out;
    for (std::size_t i = from; i < end; ++i) {
        out.push_back(
            DeadEntry{dead[i].delivery_id, dead[i].payload, dead[i].retries});
    }
    return out;
}

std::size_t GlobalManager::reliable_dead_size(const std::string& name) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = reliable_queues_.find(name);
    return it == reliable_queues_.end() ? 0 : it->second.dead.size();
}

void GlobalManager::reliable_dead_purge(const std::string& name) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = reliable_queues_.find(name);
    if (it != reliable_queues_.end()) {
        it->second.dead.clear();
    }
}

std::size_t GlobalManager::reliable_queue_count() {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    return reliable_queues_.size();
}

std::size_t GlobalManager::reliable_inflight(const std::string& name) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = reliable_queues_.find(name);
    return it == reliable_queues_.end() ? 0 : it->second.inflight.size();
}

// ---- scheduler ----

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
        if (schedule.empty() || end == nullptr ||
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
                if (task.info.paused || task.info.done ||
                    task.info.next_run_ms == 0 || task.info.next_run_ms > now) {
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
                    if (task.info.next_run_ms == 0) {
                        // No match within the horizon: retire the task.
                        task.info.done = true;
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
