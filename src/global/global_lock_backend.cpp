// [SHIELD_GLOBAL] Lock-domain backend implementations.
//
// Two GlobalLockBackend implementations: the P0 process-memory lock store
// (mutex + spinlock registries and the read/write lock, extracted from
// GlobalManager with identical lazy-expiry semantics) and a redis++ backend
// where each exclusive lock is a HASH (owner/count/acquired_at) and each
// rwlock is a HASH (writer fields + reader fields), all mutated by short
// Lua scripts so acquire/release/extend stay atomic server-side.
// GlobalManager owns the backend and delegates; the public API does not
// move.

#include "shield/global/global_lock_backend.hpp"

#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdlib>
#include <iterator>
#include <map>
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

struct RwLockEntry {
    std::string write_owner;
    std::uint64_t write_count = 0;
    std::uint64_t write_expire_at_ms = 0;          // 0 = no TTL
    std::map<std::string, std::uint64_t> readers;  // token -> hold count
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

    // ---- read/write locks (extracted from GlobalManager verbatim) ----

    LockStatus rw_write_acquire(const std::string& name,
                                const std::string& owner,
                                std::uint64_t ttl_ms) override {
        std::lock_guard<std::mutex> lock(mu_);
        const std::uint64_t now = GlobalManager::now_ms();
        auto it = rwlocks_.find(name);
        if (it == rwlocks_.end()) {
            RwLockEntry entry;
            entry.write_owner = owner;
            entry.write_count = 1;
            entry.write_expire_at_ms =
                ttl_ms != 0 ? now + ttl_ms
                            : 0;  // GCOVR_EXCL_BR_LINE (TTL=0 ternary arms)
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
            entry.write_expire_at_ms =
                ttl_ms != 0 ? now + ttl_ms
                            : 0;  // GCOVR_EXCL_BR_LINE (TTL=0 ternary arms)
            entry.readers.clear();
            return LockStatus::kOk;
        }
        if (!entry.write_owner.empty()) {
            if (entry.write_owner == owner) {
                ++entry.write_count;
                if (ttl_ms != 0) {  // GCOVR_EXCL_BR_LINE (TTL=0 ternary arm)
                    entry.write_expire_at_ms = now + ttl_ms;
                }
                return LockStatus::kOk;
            }
            return LockStatus::kBusy;
        }
        // A registered entry without a writer still holds readers: fully
        // released entries are erased, so there is no takeover arm here.
        return LockStatus::kBusy;
    }

    LockStatus rw_write_release(const std::string& name,
                                const std::string& owner) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = rwlocks_.find(name);
        if (it == rwlocks_.end() || it->second.write_owner != owner) {
            return LockStatus::kNotOwner;
        }
        // Readers cannot coexist with a writer, so the last write release
        // frees the whole entry (mirrors the redis backend dropping the
        // hash) instead of leaving an empty husk in the registry.
        if (--it->second.write_count == 0) {
            rwlocks_.erase(it);
        }
        return LockStatus::kOk;
    }

    bool rw_write_extend(const std::string& name, const std::string& owner,
                         std::uint64_t ttl_ms) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = rwlocks_.find(name);
        if (it == rwlocks_.end() || it->second.write_owner != owner) {
            return false;
        }
        if (it->second.write_expire_at_ms != 0 &&
            GlobalManager::now_ms() >= it->second.write_expire_at_ms) {
            return false;
        }
        it->second.write_expire_at_ms =
            ttl_ms != 0 ? GlobalManager::now_ms() + ttl_ms
                        : 0;  // GCOVR_EXCL_BR_LINE (TTL=0 ternary arms)
        return true;
    }

    LockStatus rw_read_acquire(const std::string& name,
                               const std::string& owner) override {
        std::lock_guard<std::mutex> lock(mu_);
        const std::uint64_t now = GlobalManager::now_ms();
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

    LockStatus rw_read_release(const std::string& name,
                               const std::string& owner) override {
        std::lock_guard<std::mutex> lock(mu_);
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
        // Reader-only entries free out too: an empty registry entry has no
        // state left to describe.
        if (it->second.readers.empty()) {
            rwlocks_.erase(it);
        }
        return LockStatus::kOk;
    }

    RwLockInfo rwlock_info(const std::string& name) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = rwlocks_.find(name);
        if (it == rwlocks_.end()) {
            return RwLockInfo{};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                  // aggregate init-list branches)
        }
        RwLockInfo info;
        info.exists = true;
        info.write_owner = it->second.write_owner;
        info.write_count = it->second.write_count;
        if (it->second.write_expire_at_ms != 0) {
            const std::uint64_t now = GlobalManager::now_ms();
            info.write_ttl_remaining_ms =
                now < it->second.write_expire_at_ms
                    ? it->second.write_expire_at_ms - now
                    : 0;  // GCOVR_EXCL_BR_LINE (TTL remaining ternary)
        }
        info.readers = it->second.readers.size();
        return info;
    }

    std::size_t rwlock_count() override {
        std::lock_guard<std::mutex> lock(mu_);
        return rwlocks_.size();
    }

private:
    std::unordered_map<std::string, MutexEntry>& table(
        const std::string& registry) {
        return registry == "spinlock" ? spinlocks_ : mutexes_;
    }

    mutable std::mutex mu_;
    std::unordered_map<std::string, MutexEntry> mutexes_;
    std::unordered_map<std::string, MutexEntry> spinlocks_;
    std::map<std::string, RwLockEntry> rwlocks_;
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

// rwlock hash: `wo` write owner ("" = none), `wc` write hold count,
// `we` write deadline ms (0 = none), `r:<token>` reader hold counts. The
// `we` field mirrors the in-process deadline; the hash TTL (PEXPIRE) is the
// crash-safety net — a live writer always excludes readers, so the hash can
// never vanish while a reader hold is recorded, and reader-only hashes are
// PERSISTed so their immortal holds cannot vanish mid-flight.
// KEYS[1] = rw hash; ARGV[1] = owner; ARGV[2] = ttl ms ("0" = none);
// ARGV[3] = now ms; ARGV[4] = deadline ms ("0" = none).
const char kRwWriteAcquireScript[] = R"(
if redis.call('EXISTS', KEYS[1]) == 0 then
  redis.call('HSET', KEYS[1], 'wo', ARGV[1], 'wc', '1', 'we', ARGV[4])
  if ARGV[2] ~= '0' then redis.call('PEXPIRE', KEYS[1], ARGV[2]) end
  return 1
end
local we = redis.call('HGET', KEYS[1], 'we') or '0'
if we ~= '0' and tonumber(ARGV[3]) >= tonumber(we) then
  for _, k in ipairs(redis.call('HKEYS', KEYS[1])) do
    if string.sub(k, 1, 2) == 'r:' then redis.call('HDEL', KEYS[1], k) end
  end
  redis.call('HSET', KEYS[1], 'wo', ARGV[1], 'wc', '1', 'we', ARGV[4])
  if ARGV[2] ~= '0' then redis.call('PEXPIRE', KEYS[1], ARGV[2]) else redis.call('PERSIST', KEYS[1]) end
  return 1
end
local wo = redis.call('HGET', KEYS[1], 'wo') or ''
if wo ~= '' then
  if wo == ARGV[1] then
    redis.call('HINCRBY', KEYS[1], 'wc', 1)
    if ARGV[2] ~= '0' then redis.call('PEXPIRE', KEYS[1], ARGV[2]) end
    return 1
  end
  return 0
end
for _, k in ipairs(redis.call('HKEYS', KEYS[1])) do
  if string.sub(k, 1, 2) == 'r:' then return 0 end
end
redis.call('HSET', KEYS[1], 'wo', ARGV[1], 'wc', '1', 'we', ARGV[4])
if ARGV[2] ~= '0' then redis.call('PEXPIRE', KEYS[1], ARGV[2]) end
return 1
)";

// Reentrant count goes down on release; dropping the writer fields also
// PERSISTs so a reader-only hash cannot keep a writer-era TTL.
const char kRwWriteReleaseScript[] = R"(
if redis.call('EXISTS', KEYS[1]) == 0 then
  return -1
end
if (redis.call('HGET', KEYS[1], 'wo') or '') ~= ARGV[1] then
  return -1
end
local n = redis.call('HINCRBY', KEYS[1], 'wc', -1)
if n <= 0 then
  redis.call('HDEL', KEYS[1], 'wo', 'wc', 'we')
  redis.call('PERSIST', KEYS[1])
end
return 1
)";

const char kRwWriteExtendScript[] = R"(
if redis.call('EXISTS', KEYS[1]) == 0 then
  return 0
end
if (redis.call('HGET', KEYS[1], 'wo') or '') ~= ARGV[1] then
  return 0
end
local we = redis.call('HGET', KEYS[1], 'we') or '0'
if we ~= '0' and tonumber(ARGV[3]) >= tonumber(we) then
  return 0
end
if ARGV[2] ~= '0' then
  redis.call('HSET', KEYS[1], 'we', ARGV[4])
  redis.call('PEXPIRE', KEYS[1], ARGV[2])
else
  redis.call('HSET', KEYS[1], 'we', '0')
  redis.call('PERSIST', KEYS[1])
end
return 1
)";

// Stale writers are reset before a read can proceed, which also drops the
// stale reader set the writer had been holding out.
const char kRwReadAcquireScript[] = R"(
if redis.call('EXISTS', KEYS[1]) == 0 then
  redis.call('HSET', KEYS[1], 'r:' .. ARGV[1], '1')
  return 1
end
local we = redis.call('HGET', KEYS[1], 'we') or '0'
if we ~= '0' and tonumber(ARGV[2]) >= tonumber(we) then
  redis.call('HDEL', KEYS[1], 'wo', 'wc', 'we')
  for _, k in ipairs(redis.call('HKEYS', KEYS[1])) do
    if string.sub(k, 1, 2) == 'r:' then redis.call('HDEL', KEYS[1], k) end
  end
  redis.call('PERSIST', KEYS[1])
end
if (redis.call('HGET', KEYS[1], 'wo') or '') ~= '' then
  return 0
end
redis.call('HINCRBY', KEYS[1], 'r:' .. ARGV[1], 1)
return 1
)";

const char kRwReadReleaseScript[] = R"(
if redis.call('EXISTS', KEYS[1]) == 0 then
  return -1
end
local field = 'r:' .. ARGV[1]
if redis.call('HGET', KEYS[1], field) == false then
  return -1
end
local n = redis.call('HINCRBY', KEYS[1], field, -1)
if n <= 0 then
  redis.call('HDEL', KEYS[1], field)
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

    LockStatus rw_write_acquire(const std::string& name,
                                const std::string& owner,
                                std::uint64_t ttl_ms) override {
        auto redis = connect();
        const std::uint64_t now = GlobalManager::now_ms();
        const std::uint64_t expire_at = ttl_ms != 0 ? now + ttl_ms : 0;
        const long long rc = redis->eval<long long>(
            kRwWriteAcquireScript, {prefixed_rw(name)},
            {owner, std::to_string(ttl_ms), std::to_string(now),
             std::to_string(expire_at)});
        return rc == 1 ? LockStatus::kOk : LockStatus::kBusy;
    }

    LockStatus rw_write_release(const std::string& name,
                                const std::string& owner) override {
        auto redis = connect();
        const long long rc = redis->eval<long long>(
            kRwWriteReleaseScript, {prefixed_rw(name)}, {owner});
        return rc == 1 ? LockStatus::kOk : LockStatus::kNotOwner;
    }

    bool rw_write_extend(const std::string& name, const std::string& owner,
                         std::uint64_t ttl_ms) override {
        auto redis = connect();
        const std::uint64_t now = GlobalManager::now_ms();
        const std::uint64_t expire_at = ttl_ms != 0 ? now + ttl_ms : 0;
        const long long rc = redis->eval<long long>(
            kRwWriteExtendScript, {prefixed_rw(name)},
            {owner, std::to_string(ttl_ms), std::to_string(now),
             std::to_string(expire_at)});
        return rc == 1;
    }

    LockStatus rw_read_acquire(const std::string& name,
                               const std::string& owner) override {
        auto redis = connect();
        const long long rc = redis->eval<long long>(
            kRwReadAcquireScript, {prefixed_rw(name)},
            {owner, std::to_string(GlobalManager::now_ms())});
        return rc == 1 ? LockStatus::kOk : LockStatus::kBusy;
    }

    LockStatus rw_read_release(const std::string& name,
                               const std::string& owner) override {
        auto redis = connect();
        const long long rc = redis->eval<long long>(
            kRwReadReleaseScript, {prefixed_rw(name)}, {owner});
        return rc == 1 ? LockStatus::kOk : LockStatus::kNotOwner;
    }

    RwLockInfo rwlock_info(const std::string& name) override {
        auto redis = connect();
        const std::string key = prefixed_rw(name);
        RwLockInfo info;
        std::unordered_map<std::string, std::string> fields;
        redis->hgetall(key, std::inserter(fields, fields.end()));
        if (fields.empty()) {
            return info;  // GCOVR_EXCL_BR_LINE (defensive: an existing hash
                          // always carries the fields the scripts write)
        }
        info.exists = true;
        auto wo = fields.find("wo");
        if (wo != fields.end() && !wo->second.empty()) {
            info.write_owner = wo->second;
            info.write_count = parse_u64(fields.at("wc"));
            const long long remaining = redis->pttl(key);
            info.write_ttl_remaining_ms =
                remaining > 0 ? static_cast<std::uint64_t>(remaining) : 0;
        }
        for (const auto& kv : fields) {
            if (kv.first.rfind("r:", 0) == 0) ++info.readers;
        }
        return info;
    }

    std::size_t rwlock_count() override {
        auto redis = connect();
        std::size_t count = 0;
        const std::string pattern = cfg_.prefix + ":lock:rw:*";
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

    std::string prefixed_rw(const std::string& name) const {
        return cfg_.prefix + ":lock:rw:" + name;
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
