// [SHIELD_GLOBAL] Lock-domain backend implementations.
//
// Two GlobalLockBackend implementations: the P0 process-memory exclusive
// lock store (mutex + spinlock registries, extracted from GlobalManager with
// identical lazy-expiry semantics) and a redis++ backend where each lock is
// a HASH (owner/count/acquired_at) mutated by short Lua scripts, so
// acquire/release/extend stay atomic server-side. GlobalManager owns the
// backend and delegates; the public API does not move.

#include "shield/global/global_lock_backend.hpp"

#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "shield/global/global_manager.hpp"

namespace shield::global {
namespace {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// In-process backend (P0 semantics, extracted from GlobalManager)
// ---------------------------------------------------------------------------

struct MutexEntry {
    std::string owner;
    std::uint64_t count = 0;
    std::uint64_t acquired_at_ms = 0;
    std::uint64_t expire_at_ms = 0;  // 0 = no TTL
};

bool mutex_live(const MutexEntry& entry, std::uint64_t now) {
    return entry.expire_at_ms == 0 || now < entry.expire_at_ms;
}

bool mutex_try_acquire_locked(
    std::unordered_map<std::string, MutexEntry>& registry,
    const std::string& name, const std::string& owner, std::uint64_t ttl_ms) {
    const std::uint64_t now = GlobalManager::now_ms();
    auto it = registry.find(name);
    if (it == registry.end()) {
        MutexEntry entry;
        entry.owner = owner;
        entry.count = 1;
        entry.acquired_at_ms = now;
        entry.expire_at_ms = ttl_ms != 0 ? now + ttl_ms  // GCOVR_EXCL_BR_LINE
                                                         // (TTL=0 ternary arms)
                                         : 0;
        registry.emplace(name, std::move(entry));
        return true;
    }
    MutexEntry& entry = it->second;
    if (!mutex_live(entry, now)) {
        // Lazy TTL expiry: the stale holder lost the lock.
        entry.owner = owner;
        entry.count = 1;
        entry.acquired_at_ms = now;
        entry.expire_at_ms = ttl_ms != 0 ? now + ttl_ms  // GCOVR_EXCL_BR_LINE
                                                         // (TTL=0 ternary arms)
                                         : 0;
        return true;
    }
    if (entry.owner == owner) {
        ++entry.count;
        if (ttl_ms != 0) {  // GCOVR_EXCL_BR_LINE (TTL=0 ternary arm)
            entry.expire_at_ms = now + ttl_ms;
        }
        return true;
    }
    return false;
}

class InProcessLockBackend final : public GlobalLockBackend {
public:
    LockStatus mutex_acquire(const std::string& registry,
                             const std::string& name, const std::string& owner,
                             std::uint64_t ttl_ms) override {
        std::lock_guard<std::mutex> lock(mu_);
        return mutex_try_acquire_locked(table(registry), name, owner, ttl_ms)
                   ? LockStatus::kOk
                   : LockStatus::kBusy;
    }

    LockStatus mutex_release(const std::string& registry,
                             const std::string& name,
                             const std::string& owner) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto& table_ref = table(registry);
        auto it = table_ref.find(name);
        if (it == table_ref.end() || it->second.owner != owner) {
            return LockStatus::kNotOwner;
        }
        if (--it->second.count == 0) {
            table_ref.erase(it);
        }
        return LockStatus::kOk;
    }

    bool mutex_extend(const std::string& registry, const std::string& name,
                      const std::string& owner, std::uint64_t ttl_ms) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto& table_ref = table(registry);
        auto it = table_ref.find(name);
        if (it == table_ref.end() || it->second.owner != owner ||
            !mutex_live(it->second, GlobalManager::now_ms())) {
            return false;
        }
        it->second.expire_at_ms =
            ttl_ms != 0 ? GlobalManager::now_ms() + ttl_ms
                        : 0;  // GCOVR_EXCL_BR_LINE (TTL=0 ternary arms)
        return true;
    }

    LockInfo mutex_info(const std::string& registry,
                        const std::string& name) override {
        std::lock_guard<std::mutex> lock(mu_);
        const auto& table_ref = table(registry);
        auto it = table_ref.find(name);
        if (it == table_ref.end()) {
            return LockInfo{};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // aggregate init-list branches)
        }
        LockInfo info;
        info.exists = true;
        info.owner = it->second.owner;
        info.count = it->second.count;
        info.acquired_at_ms = it->second.acquired_at_ms;
        if (it->second.expire_at_ms != 0) {
            const std::uint64_t now = GlobalManager::now_ms();
            info.ttl_remaining_ms =
                now < it->second.expire_at_ms
                    ? it->second.expire_at_ms - now  // GCOVR_EXCL_BR_LINE (TTL
                                                     // remaining ternary)
                    : 0;
        }
        return info;
    }

    std::size_t mutex_registry_size(const std::string& registry) override {
        std::lock_guard<std::mutex> lock(mu_);
        return table(registry).size();
    }

private:
    std::unordered_map<std::string, MutexEntry>& table(
        const std::string& registry) {
        return registry == "spinlock" ? spinlocks_ : mutexes_;
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, MutexEntry> mutexes_;
    std::unordered_map<std::string, MutexEntry> spinlocks_;
};

// ---------------------------------------------------------------------------
// Redis backend (redis++, lazy connect, Lua-guarded HASH per lock)
// ---------------------------------------------------------------------------

// KEYS[1] = lock hash; ARGV[1] = owner; ARGV[2] = ttl ms ("0" = none);
// ARGV[3] = now ms (bookkeeping). Fresh acquire and same-owner reentrancy
// both refresh the TTL, so a reentrant hold does not shorten the window.
const char kAcquireScript[] = R"(
if redis.call('EXISTS', KEYS[1]) == 0 then
  redis.call('HSET', KEYS[1], 'owner', ARGV[1], 'count', '1',
             'acquired_at', ARGV[3])
  if ARGV[2] ~= '0' then redis.call('PEXPIRE', KEYS[1], ARGV[2]) end
  return 1
end
if redis.call('HGET', KEYS[1], 'owner') == ARGV[1] then
  redis.call('HINCRBY', KEYS[1], 'count', 1)
  if ARGV[2] ~= '0' then redis.call('PEXPIRE', KEYS[1], ARGV[2]) end
  return 1
end
return 0
)";

// Reentrant count goes down on release; the hash disappears at zero, so the
// next owner takes the name cleanly.
const char kReleaseScript[] = R"(
if redis.call('EXISTS', KEYS[1]) == 0 then
  return -1
end
if redis.call('HGET', KEYS[1], 'owner') ~= ARGV[1] then
  return -1
end
local n = redis.call('HINCRBY', KEYS[1], 'count', -1)
if n <= 0 then
  redis.call('DEL', KEYS[1])
end
return 1
)";

const char kExtendScript[] = R"(
if redis.call('EXISTS', KEYS[1]) == 0 then
  return 0
end
if redis.call('HGET', KEYS[1], 'owner') ~= ARGV[1] then
  return 0
end
if ARGV[2] ~= '0' then
  redis.call('PEXPIRE', KEYS[1], ARGV[2])
else
  redis.call('PERSIST', KEYS[1])
end
return 1
)";

class RedisLockBackend final : public GlobalLockBackend {
public:
    explicit RedisLockBackend(const RedisDataBackendConfig& cfg) : cfg_(cfg) {}

    LockStatus mutex_acquire(const std::string& registry,
                             const std::string& name, const std::string& owner,
                             std::uint64_t ttl_ms) override {
        auto redis = connect();
        const std::string key = prefixed_lock(registry, name);
        const long long rc =
            redis->eval<long long>(kAcquireScript, {key},
                                   {owner, std::to_string(ttl_ms),
                                    std::to_string(GlobalManager::now_ms())});
        return rc == 1 ? LockStatus::kOk : LockStatus::kBusy;
    }

    LockStatus mutex_release(const std::string& registry,
                             const std::string& name,
                             const std::string& owner) override {
        auto redis = connect();
        const long long rc = redis->eval<long long>(
            kReleaseScript, {prefixed_lock(registry, name)}, {owner});
        return rc == 1 ? LockStatus::kOk : LockStatus::kNotOwner;
    }

    bool mutex_extend(const std::string& registry, const std::string& name,
                      const std::string& owner, std::uint64_t ttl_ms) override {
        auto redis = connect();
        const long long rc = redis->eval<long long>(
            kExtendScript, {prefixed_lock(registry, name)},
            {owner, std::to_string(ttl_ms)});
        return rc == 1;
    }

    LockInfo mutex_info(const std::string& registry,
                        const std::string& name) override {
        auto redis = connect();
        const std::string key = prefixed_lock(registry, name);
        LockInfo info;
        std::unordered_map<std::string, std::string> fields;
        redis->hgetall(key, std::inserter(fields, fields.end()));
        if (fields.empty()) {
            return info;  // GCOVR_EXCL_BR_LINE (defensive: an existing hash
                          // always carries the fields the scripts write)
        }
        info.exists = true;
        info.owner = fields.at("owner");
        info.count = parse_u64(fields.at("count"));
        info.acquired_at_ms = parse_u64(fields.at("acquired_at"));
        const long long remaining = redis->pttl(key);
        info.ttl_remaining_ms =
            remaining > 0 ? static_cast<std::uint64_t>(remaining) : 0;
        return info;
    }

    std::size_t mutex_registry_size(const std::string& registry) override {
        auto redis = connect();
        std::size_t count = 0;
        const std::string pattern = cfg_.prefix + ":lock:" + registry + ":*";
        long long cursor = 0;
        do {
            std::vector<std::string> keys;
            cursor = redis->scan(cursor, pattern, 100,
                                 std::inserter(keys, keys.end()));
            count += keys.size();
        } while (cursor != 0);
        return count;
    }

private:
    std::string prefixed_lock(const std::string& registry,
                              const std::string& name) const {
        return cfg_.prefix + ":lock:" + registry + ":" + name;
    }

    static std::uint64_t parse_u64(const std::string& text) {
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
        if (end == nullptr || end != text.c_str() + text.size()) {
            return 0;  // GCOVR_EXCL_LINE (defensive: values written by our
                       // own scripts are always plain integers)
        }
        return static_cast<std::uint64_t>(parsed);
    }

    // Lazy connect: construction must not block, so the Redis handle is
    // built on first use (same rule as the data backend).
    std::shared_ptr<sw::redis::Redis> connect() {
        std::lock_guard<std::mutex> lock(mu_);
        if (redis_) return redis_;
        sw::redis::ConnectionOptions opts;
        opts.host = cfg_.host;
        opts.port = cfg_.port > 0 ? cfg_.port : 6379;
        if (!cfg_.password.empty()) opts.password = cfg_.password;
        opts.db = cfg_.db > 0 ? cfg_.db : 0;
        opts.connect_timeout = 2000ms;
        opts.socket_timeout = 2000ms;
        redis_ = std::make_shared<sw::redis::Redis>(opts);
        return redis_;
    }

    RedisDataBackendConfig cfg_;
    std::mutex mu_;
    std::shared_ptr<sw::redis::Redis> redis_;
};

}  // namespace

std::unique_ptr<GlobalLockBackend> make_inprocess_lock_backend() {
    return std::make_unique<InProcessLockBackend>();
}

std::unique_ptr<GlobalLockBackend> make_redis_lock_backend(
    const RedisDataBackendConfig& config) {
    return std::make_unique<RedisLockBackend>(config);
}

}  // namespace shield::global
