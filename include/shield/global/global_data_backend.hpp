// [SHIELD_GLOBAL] Data-domain backend seam.
//
// The global data (KV, JSON text values with optional TTL) lives behind this
// interface: the P0 backend is process memory, a Redis backend slots in via
// `global.data_backend = redis` plus the `global.redis.*` keys. The Lua
// surface (shield.global() KV ops) is untouched — GlobalManager delegates
// every data_* call to the backend and keeps the local-cache coherence
// rules (a write or a lazy expiry drops the cached copy) in its delegation
// layer, so the backends know nothing about the cache.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace shield::global {

/// Connection parameters for the Redis data backend (`global.redis.*`).
struct RedisDataBackendConfig {
    std::string host;
    int port = 6379;
    std::string password;
    int db = 0;
    /// Every key is stored as `<prefix>:<key>`; separate deployments share
    /// one Redis instance by using different prefixes.
    std::string prefix = "shield:global";
};

/// Storage seam for the global data domain (KV + optional TTL per key).
/// Implementations must be safe for concurrent use.
class GlobalDataBackend {
public:
    virtual ~GlobalDataBackend() = default;

    /// Returns false when the key is absent; a lazily expired entry is
    /// dropped by the backend before returning false.
    virtual bool get(const std::string& key, std::string* out) = 0;
    /// ttl_ms 0 = no expiry (clears any TTL a previous write left, like the
    /// in-process domain). The Redis backend maps ttl_ms to PEXPIRE.
    virtual void set(const std::string& key, std::string value,
                     std::uint64_t ttl_ms) = 0;
    /// Returns true when a live entry was removed.
    virtual bool del(const std::string& key) = 0;
    /// Read-modify-write an integer value. Missing keys start at 0; a
    /// non-integer current value fails with `error` set and no write.
    virtual bool incr_by(const std::string& key, std::int64_t delta,
                         std::int64_t* out, std::string* error) = 0;
    /// Positional multi-get; a missing key yields std::nullopt.
    virtual std::vector<std::optional<std::string>> mget(
        const std::vector<std::string>& keys) = 0;
    /// Writes every pair (callers validate keys first; no empty keys here).
    virtual bool mset(
        const std::vector<std::pair<std::string, std::string>>& kvs,
        std::uint64_t ttl_ms, std::string* error) = 0;
    /// Number of live entries visible to this backend.
    virtual std::size_t size() = 0;
};

/// The P0 process-memory backend (extracted from GlobalManager).
std::unique_ptr<GlobalDataBackend> make_inprocess_data_backend();

/// redis++ backend (lazy connect: construction never blocks).
std::unique_ptr<GlobalDataBackend> make_redis_data_backend(
    const RedisDataBackendConfig& config);

}  // namespace shield::global
