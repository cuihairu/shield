// [SHIELD_GLOBAL] Lock-domain backend seam.
//
// The exclusive-lock domain (mutex + spinlock registries; the
// distributed_mutex facade shares the mutex registry) sits behind this
// interface, mirroring GlobalDataBackend: GlobalManager owns a backend and
// delegates, keeping the domain types and the public API on the manager.
// The in-process implementation is the P0 store; the redis++ implementation
// uses a per-lock HASH (owner/count/acquired_at) guarded by small Lua
// scripts so acquire/release/extend stay atomic server-side.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "shield/global/global_data_backend.hpp"

namespace shield::global {

/// Locks. `owner` is an opaque token minted by the Lua facade (per lock
/// object); reentrancy keys on it. TTL expiry is lazy: an expired lock is
/// taken over by the next acquirer.
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

class GlobalLockBackend {
public:
    virtual ~GlobalLockBackend() = default;

    virtual LockStatus mutex_acquire(const std::string& registry,
                                     const std::string& name,
                                     const std::string& owner,
                                     std::uint64_t ttl_ms) = 0;
    virtual LockStatus mutex_release(const std::string& registry,
                                     const std::string& name,
                                     const std::string& owner) = 0;
    virtual bool mutex_extend(const std::string& registry,
                              const std::string& name, const std::string& owner,
                              std::uint64_t ttl_ms) = 0;
    virtual LockInfo mutex_info(const std::string& registry,
                                const std::string& name) = 0;
    virtual std::size_t mutex_registry_size(const std::string& registry) = 0;
};

std::unique_ptr<GlobalLockBackend> make_inprocess_lock_backend();
std::unique_ptr<GlobalLockBackend> make_redis_lock_backend(
    const RedisDataBackendConfig& config);

}  // namespace shield::global
