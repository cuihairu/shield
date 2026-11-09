// [SHIELD_GLOBAL] GlobalManager lock + rank domains: mutex/rwlock
// registries over the lock backend and leaderboard operations over
// the rank backend.
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

void GlobalManager::set_lock_backend(
    std::unique_ptr<GlobalLockBackend> backend) {
    if (!backend) {
        return;
    }
    std::lock_guard<std::mutex> lock(locks_mutex_);
    lock_backend_ = std::move(backend);
}

void GlobalManager::set_rank_backend(
    std::unique_ptr<GlobalRankBackend> backend) {
    if (!backend) {
        return;
    }
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    rank_backend_ = std::move(backend);
}

LockStatus GlobalManager::mutex_acquire(const std::string& registry,
                                        const std::string& name,
                                        const std::string& owner,
                                        std::uint64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    return lock_backend_->mutex_acquire(registry, name, owner, ttl_ms);
}

LockStatus GlobalManager::mutex_release(const std::string& registry,
                                        const std::string& name,
                                        const std::string& owner) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    return lock_backend_->mutex_release(registry, name, owner);
}

bool GlobalManager::mutex_extend(const std::string& registry,
                                 const std::string& name,
                                 const std::string& owner,
                                 std::uint64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    return lock_backend_->mutex_extend(registry, name, owner, ttl_ms);
}

LockInfo GlobalManager::mutex_info(const std::string& registry,
                                   const std::string& name) const {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    return lock_backend_->mutex_info(registry, name);
}

std::size_t GlobalManager::mutex_registry_size(
    const std::string& registry) const {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    return lock_backend_->mutex_registry_size(registry);
}

// ---- rwlocks ----

LockStatus GlobalManager::rw_write_acquire(const std::string& name,
                                           const std::string& owner,
                                           std::uint64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    return lock_backend_->rw_write_acquire(name, owner, ttl_ms);
}

LockStatus GlobalManager::rw_write_release(const std::string& name,
                                           const std::string& owner) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    return lock_backend_->rw_write_release(name, owner);
}

bool GlobalManager::rw_write_extend(const std::string& name,
                                    const std::string& owner,
                                    std::uint64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    return lock_backend_->rw_write_extend(name, owner, ttl_ms);
}

LockStatus GlobalManager::rw_read_acquire(const std::string& name,
                                          const std::string& owner) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    return lock_backend_->rw_read_acquire(name, owner);
}

LockStatus GlobalManager::rw_read_release(const std::string& name,
                                          const std::string& owner) {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    return lock_backend_->rw_read_release(name, owner);
}

RwLockInfo GlobalManager::rwlock_info(const std::string& name) const {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    return lock_backend_->rwlock_info(name);
}

std::size_t GlobalManager::rwlock_count() const {
    std::lock_guard<std::mutex> lock(locks_mutex_);
    return lock_backend_->rwlock_count();
}

// ---- leaderboards ----
//
// Every rank_* method delegates to the GlobalRankBackend seam. ranks_mutex_
// guards the delegation and backend swap, so a backend swap cannot race a
// query in flight (the backend takes its own locks inside).

void GlobalManager::rank_update(const std::string& board,
                                const std::string& uid, double score) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    rank_backend_->rank_update(board, uid, score);
}

void GlobalManager::rank_mupdate(
    const std::string& board,
    const std::vector<std::pair<std::string, double>>& updates) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    rank_backend_->rank_mupdate(board, updates);
}

std::optional<double> GlobalManager::rank_score(const std::string& board,
                                                const std::string& uid) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    return rank_backend_->rank_score(board, uid);
}

std::optional<std::uint64_t> GlobalManager::rank_position(
    const std::string& board, const std::string& uid) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    return rank_backend_->rank_position(board, uid);
}

std::vector<RankEntry> GlobalManager::rank_top(const std::string& board,
                                               std::size_t n) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    return rank_backend_->rank_top(board, n);
}

std::vector<RankEntry> GlobalManager::rank_range(const std::string& board,
                                                 std::uint64_t from,
                                                 std::uint64_t to) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    return rank_backend_->rank_range(board, from, to);
}

std::vector<RankEntry> GlobalManager::rank_range_by_score(
    const std::string& board, double lo, double hi) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    return rank_backend_->rank_range_by_score(board, lo, hi);
}

RankAround GlobalManager::rank_around(const std::string& board,
                                      const std::string& uid, std::size_t n) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    return rank_backend_->rank_around(board, uid, n);
}

std::size_t GlobalManager::rank_count(const std::string& board) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    return rank_backend_->rank_count(board);
}

bool GlobalManager::rank_remove(const std::string& board,
                                const std::string& uid) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    return rank_backend_->rank_remove(board, uid);
}

void GlobalManager::rank_clear(const std::string& board) {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    rank_backend_->rank_clear(board);
}

std::size_t GlobalManager::rank_board_count() {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    return rank_backend_->rank_board_count();
}

std::size_t GlobalManager::rank_total_members() {
    std::lock_guard<std::mutex> lock(ranks_mutex_);
    return rank_backend_->rank_total_members();
}

// ---- normal queues ----

}  // namespace shield::global
