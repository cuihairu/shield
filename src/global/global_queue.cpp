// [SHIELD_GLOBAL] GlobalManager queue domains: FIFO queues, delayed
// queues and priority queues.
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

// ---- priority queues ----

void GlobalManager::priority_push(const std::string& name, std::string payload,
                                  std::int64_t priority) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    priority_queues_[name][priority].push_back(std::move(payload));
}

bool GlobalManager::priority_pop(const std::string& name, std::string* out) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = priority_queues_.find(name);
    if (it == priority_queues_.end()) {
        return false;
    }
    // Lowest level first; erase levels drained to empty so a burst of one
    // shot priorities does not leave empty buckets behind.
    auto& levels = it->second;
    for (auto lit = levels.begin(); lit != levels.end(); ++lit) {
        auto& bucket = lit->second;
        // Defensive: drained levels are erased in place, so an empty
        // bucket never survives to the next pop.
        if (bucket.empty()) {  // GCOVR_EXCL_BR_LINE (defensive)
            continue;          // GCOVR_EXCL_LINE (unreachable)
        }
        if (out) {
            *out = std::move(bucket.front());
        }
        bucket.pop_front();
        if (bucket.empty()) {
            levels.erase(lit);
        }
        return true;
    }
    return false;
}

std::size_t GlobalManager::priority_length(const std::string& name) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = priority_queues_.find(name);
    if (it == priority_queues_.end()) {
        return 0;
    }
    std::size_t total = 0;
    for (const auto& [priority, bucket] : it->second) {
        (void)priority;
        total += bucket.size();
    }
    return total;
}

void GlobalManager::priority_purge(const std::string& name) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    priority_queues_.erase(name);
}

std::size_t GlobalManager::priority_queue_count() {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    return priority_queues_.size();
}

// ---- broadcast queues ----

}  // namespace shield::global
