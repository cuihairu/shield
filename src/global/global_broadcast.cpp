// [SHIELD_GLOBAL] GlobalManager broadcast + reliable-delivery
// domains: group broadcast with replay windows and the reliable
// queue with ack/nack and dead-letter handling.
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

void GlobalManager::broadcast_configure(const std::string& name,
                                        std::size_t max_history) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    broadcast_queues_[name].max_history = max_history;
}

std::uint64_t GlobalManager::broadcast_push(const std::string& name,
                                            std::string payload) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto& queue = broadcast_queues_[name];
    const std::uint64_t seq = queue.next_seq++;
    queue.history.emplace_back(seq, std::move(payload));
    while (queue.history.size() > queue.max_history) {
        queue.history.pop_front();
    }
    return seq;
}

std::uint64_t GlobalManager::broadcast_attach(const std::string& name,
                                              const std::string& group) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto& queue = broadcast_queues_[name];
    auto [it, inserted] = queue.group_cursors.try_emplace(
        group, queue.next_seq == 0 ? 0 : queue.next_seq - 1);
    (void)inserted;
    return it->second;
}

std::uint64_t GlobalManager::broadcast_since(const std::string& name,
                                             const std::string& group,
                                             std::vector<std::string>* out) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto qit = broadcast_queues_.find(name);
    if (qit == broadcast_queues_.end()) {
        return 0;
    }
    auto& queue = qit->second;
    auto git = queue.group_cursors.find(group);
    if (git == queue.group_cursors.end()) {
        return 0;  // unknown group: nothing to replay
    }
    const std::uint64_t cursor = git->second;
    std::uint64_t last = 0;
    for (const auto& [seq, payload] : queue.history) {
        if (seq <= cursor) {
            continue;
        }
        if (out) {  // GCOVR_EXCL_BR_LINE (null out-param arm)
            out->push_back(payload);
        }
        last = seq;
    }
    return last;
}

void GlobalManager::broadcast_commit(const std::string& name,
                                     const std::string& group,
                                     std::uint64_t seq) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto qit = broadcast_queues_.find(name);
    if (qit == broadcast_queues_.end()) {
        return;
    }
    auto& cursors = qit->second.group_cursors;
    auto [it, inserted] = cursors.try_emplace(group, seq);
    if (!inserted && seq > it->second) {
        it->second = seq;
    }
}

std::size_t GlobalManager::broadcast_history_size(const std::string& name) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = broadcast_queues_.find(name);
    return it == broadcast_queues_.end() ? 0 : it->second.history.size();
}

std::size_t GlobalManager::broadcast_group_count(const std::string& name) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    auto it = broadcast_queues_.find(name);
    return it == broadcast_queues_.end() ? 0 : it->second.group_cursors.size();
}

void GlobalManager::broadcast_purge(const std::string& name) {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    broadcast_queues_.erase(name);
}

std::size_t GlobalManager::broadcast_queue_count() {
    std::lock_guard<std::mutex> lock(queues_mutex_);
    return broadcast_queues_.size();
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
    if (it == reliable_queues_.end() ||
        it->second.pending.empty() ||  // GCOVR_EXCL_BR_LINE (compound
                                       // condition: 3 parts exercised by tests)
        it->second.pending.begin()->first > now_ms()) {
        return false;
    }
    auto& queue = it->second;
    auto first = queue.pending.begin();
    const std::uint64_t retries =
        queue.retry_counts.count(first->second.id) !=
                0  // GCOVR_EXCL_BR_LINE (ternary arms exercised by tests)
            ? queue.retry_counts[first->second.id]
            : 0;
    DeadItem item{first->second.id, std::move(first->second.payload),
                  retries};  // GCOVR_EXCL_BR_LINE (compiler artifact: aggregate
                             // init-list branches)
    queue.pending.erase(first);
    const std::uint64_t delivery_id = item.delivery_id;
    queue.inflight.emplace(delivery_id, std::move(item));
    if (out) {  // GCOVR_EXCL_BR_LINE (null out-param arm)
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
    if (it == reliable_queues_.end() ||
        from >=
            it->second.dead.size()) {  // GCOVR_EXCL_BR_LINE (compound
                                       // condition: 2 parts exercised by tests)
        return {};
    }
    const auto& dead = it->second.dead;
    const std::size_t end = std::min<std::size_t>(to + 1, dead.size());
    std::vector<DeadEntry> out;
    for (std::size_t i = from; i < end; ++i) {
        out.push_back(DeadEntry{
            // GCOVR_EXCL_BR_LINE (compiler artifact:
            // aggregate init-list branches)
            dead[i].delivery_id, dead[i].payload, dead[i].retries});
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

}  // namespace shield::global
