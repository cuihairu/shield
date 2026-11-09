// [SHIELD_GLOBAL] GlobalManager data + cache domains: the key-value
// passthrough over the data backend and the TTL cache with hit/miss
// counters.
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

void GlobalManager::set_data_backend(
    std::unique_ptr<GlobalDataBackend> backend) {
    if (!backend) {
        return;
    }
    std::lock_guard<std::mutex> lock(data_mutex_);
    data_backend_ = std::move(backend);
}

bool GlobalManager::data_get(const std::string& key, std::string* out) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    const bool found = data_backend_->get(key, out);
    if (!found) {
        // A lazily expired entry must also drop the cached copy.
        cache_drop_locked(key);
    }
    return found;
}

void GlobalManager::data_set(const std::string& key, std::string value,
                             std::uint64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    data_backend_->set(key, std::move(value), ttl_ms);
    // Writes invalidate the cached copy (cache coherence contract).
    cache_drop_locked(key);
}

bool GlobalManager::data_delete(const std::string& key) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    const bool erased = data_backend_->del(key);
    cache_drop_locked(key);
    return erased;
}

bool GlobalManager::data_incr_by(const std::string& key, std::int64_t delta,
                                 std::int64_t* out, std::string* error) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    if (!data_backend_->incr_by(key, delta, out, error)) {
        return false;
    }
    // An increment is a write: drop the cached copy.
    cache_drop_locked(key);
    return true;
}

std::vector<std::optional<std::string>> GlobalManager::data_mget(
    const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return data_backend_->mget(keys);
}

bool GlobalManager::data_mset(
    const std::vector<std::pair<std::string, std::string>>& kvs,
    std::uint64_t ttl_ms, std::string* error) {
    for (const auto& kv : kvs) {
        if (kv.first.empty()) {
            if (error)
                *error =
                    "mset key must not be empty";  // GCOVR_EXCL_BR_LINE (null
                                                   // error out-param arm)
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(data_mutex_);
    return data_backend_->mset(kvs, ttl_ms, error);
}

std::size_t GlobalManager::data_size() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return data_backend_->size();
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
            if (out) {  // GCOVR_EXCL_BR_LINE (null out-param arm)
                *out = idx->second->second;
            }
            return true;
        }
    }
    ++cache_misses_;
    // Miss: fill from the backend. A cache entry for the key cannot exist
    // here (an expired one was dropped above); the backend's lazy expiry
    // already ran when its get() returned false.
    std::string value;
    if (!data_backend_->get(key, &value)) {
        return false;
    }
    const std::uint64_t effective_ttl =
        ttl_ms != 0 ? ttl_ms
                    : config_.cache_default_ttl_ms;  // GCOVR_EXCL_BR_LINE
                                                     // (TTL=0 ternary arms)
    cache_lru_.emplace_front(key, value);
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
    if (out) {  // GCOVR_EXCL_BR_LINE (null out-param arm)
        *out = value;
    }
    return true;
}

void GlobalManager::cache_drop_locked(const std::string& key) {
    auto idx = cache_index_.find(key);
    if (idx != cache_index_.end()) {
        cache_lru_.erase(idx->second);
        cache_index_.erase(idx);
        cache_expire_.erase(key);
    }
}

void GlobalManager::cache_invalidate(const std::string& key) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    cache_drop_locked(key);
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
//
// Every mutex_* method delegates to the GlobalLockBackend seam. The
// locks_mutex_ guards the delegation AND the read/write registries below, so
// the manager's view of a lock cannot interleave with another thread's
// write (the backend takes its own locks inside).

}  // namespace shield::global
