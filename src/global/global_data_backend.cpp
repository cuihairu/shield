// [SHIELD_GLOBAL] Data-domain backend implementations.
//
// Two GlobalDataBackend implementations: the P0 process-memory store
// (extracted from GlobalManager with identical lazy-expiry semantics) and a
// redis++ backend (lazy connect; PEXPIRE for TTLs, INCRBY for counters,
// SCAN-with-prefix for size). GlobalManager owns the backend and keeps the
// cache-coherence rules in its delegation layer.

#include "shield/global/global_data_backend.hpp"

#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "shield/global/global_manager.hpp"

namespace shield::global {
namespace {

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// In-process backend (P0 semantics, extracted verbatim from GlobalManager)
// ---------------------------------------------------------------------------

class InProcessDataBackend final : public GlobalDataBackend {
public:
    bool get(const std::string& key, std::string* out) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = data_.find(key);
        if (it == data_.end()) {
            return false;
        }
        auto exp = expire_.find(key);
        if (exp != expire_.end() && exp->second <= GlobalManager::now_ms()) {
            data_.erase(it);
            expire_.erase(exp);
            return false;
        }
        if (out) {
            *out = it->second;
        }
        return true;
    }

    void set(const std::string& key, std::string value,
             std::uint64_t ttl_ms) override {
        std::lock_guard<std::mutex> lock(mu_);
        data_[key] = std::move(value);
        if (ttl_ms != 0) {
            expire_[key] = GlobalManager::now_ms() + ttl_ms;
        } else {
            expire_.erase(key);
        }
    }

    bool del(const std::string& key) override {
        std::lock_guard<std::mutex> lock(mu_);
        const bool erased = data_.erase(key) != 0;
        expire_.erase(key);
        return erased;
    }

    bool incr_by(const std::string& key, std::int64_t delta, std::int64_t* out,
                 std::string* error) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto exp = expire_.find(key);
        if (exp != expire_.end() && exp->second <= GlobalManager::now_ms()) {
            data_.erase(key);
            expire_.erase(exp);
        }
        auto it = data_.find(key);
        std::int64_t current = 0;
        if (it != data_.end()) {
            const std::string& text = it->second;
            char* end = nullptr;
            const long long parsed = std::strtoll(text.c_str(), &end, 10);
            if (text.empty() ||    // GCOVR_EXCL_BR_LINE (defensive: glibc
                                   // strtoll always stores a non-null
                                   // end, so the end==nullptr arm is
                                   // unreachable; the remaining arms are
                                   // parse-guard variants)
                end == nullptr ||  // GCOVR_EXCL_BR_LINE (defensive: glibc
                                   // strtoll always stores a non-null
                                   // end; unparsable tails are driven by
                                   // BackendIncrByRejectsNonIntegers)
                end != text.c_str() + text.size()) {
                if (error) {  // GCOVR_EXCL_BR_LINE (null error out-param arm)
                    *error = "value of '" + key + "' is not an integer";
                }
                return false;
            }
            current = static_cast<std::int64_t>(parsed);
        }
        current += delta;
        data_[key] = std::to_string(current);
        if (out) {  // GCOVR_EXCL_BR_LINE (null out-param arm)
            *out = current;
        }
        return true;
    }

    std::vector<std::optional<std::string>> mget(
        const std::vector<std::string>& keys) override {
        std::vector<std::optional<std::string>> values;
        values.reserve(keys.size());
        for (const auto& key : keys) {
            std::string value;
            if (get(key, &value)) {
                values.emplace_back(std::move(value));
            } else {
                values.emplace_back();
            }
        }
        return values;
    }  // GCOVR_EXCL_LINE (gcov epilogue artifact: exit block of mget never
       // counted)

    bool mset(const std::vector<std::pair<std::string, std::string>>& kvs,
              std::uint64_t ttl_ms, std::string* error) override {
        (void)error;
        for (const auto& kv : kvs) {
            set(kv.first, kv.second, ttl_ms);
        }
        return true;
    }

    std::size_t size() override {
        std::lock_guard<std::mutex> lock(mu_);
        return data_.size();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::uint64_t> expire_;  // key -> ts
    std::unordered_map<std::string, std::string> data_;
};

// ---------------------------------------------------------------------------
// Redis backend (redis++, lazy connect)
// ---------------------------------------------------------------------------

class RedisDataBackend final : public GlobalDataBackend {
public:
    explicit RedisDataBackend(const RedisDataBackendConfig& cfg) : cfg_(cfg) {}

    bool get(const std::string& key, std::string* out) override {
        auto redis = connect();
        const auto value = redis->get(prefixed(key));
        if (!value) {
            return false;
        }
        if (out) {
            *out = *value;
        }
        return true;
    }

    void set(const std::string& key, std::string value,
             std::uint64_t ttl_ms) override {
        auto redis = connect();
        const std::string k = prefixed(key);
        if (ttl_ms != 0) {
            redis->set(k, value, std::chrono::milliseconds(ttl_ms));
        } else {
            redis->set(k, value);
            // ttl 0 means "never expires": clear any TTL a previous write
            // left on the key (same rule as the in-process domain).
            redis->persist(k);
        }
    }

    bool del(const std::string& key) override {
        auto redis = connect();
        return redis->del(prefixed(key)) > 0;
    }

    bool incr_by(const std::string& key, std::int64_t delta, std::int64_t* out,
                 std::string* error) override {
        auto redis = connect();
        try {
            const std::int64_t value = redis->incrby(prefixed(key), delta);
            if (out) {  // GCOVR_EXCL_BR_LINE (null out-param arm)
                *out = value;
            }
            return true;
        } catch (const sw::redis::Error&) {
            // Redis refuses INCRBY on a non-integer value with an error
            // reply; surface the same message the in-process domain uses.
            if (error) {  // GCOVR_EXCL_BR_LINE (null error out-param arm)
                *error = "value of '" + key + "' is not an integer";
            }
            return false;
        }
    }

    std::vector<std::optional<std::string>> mget(
        const std::vector<std::string>& keys) override {
        if (keys.empty()) {
            return {};
        }
        auto redis = connect();
        std::vector<std::string> prefixed_keys;
        prefixed_keys.reserve(keys.size());
        for (const auto& key : keys) {
            prefixed_keys.push_back(prefixed(key));
        }
        // redis++ (vcpkg build) parses MGET replies into its own
        // OptionalString, which mirrors std::optional one-to-one.
        std::vector<sw::redis::OptionalString> values;
        redis->mget(prefixed_keys.begin(), prefixed_keys.end(),
                    std::back_inserter(values));
        std::vector<std::optional<std::string>> out;
        out.reserve(values.size());
        for (const auto& value : values) {
            if (value) {
                out.emplace_back(*value);
            } else {
                out.emplace_back();
            }
        }
        return out;
    }

    bool mset(const std::vector<std::pair<std::string, std::string>>& kvs,
              std::uint64_t ttl_ms, std::string* error) override {
        if (kvs.empty()) {
            return true;
        }
        auto redis = connect();
        std::vector<std::pair<std::string, std::string>> prefixed_kvs;
        prefixed_kvs.reserve(kvs.size());
        for (const auto& kv : kvs) {
            prefixed_kvs.emplace_back(prefixed(kv.first), kv.second);
        }
        redis->mset(prefixed_kvs.begin(), prefixed_kvs.end());
        if (ttl_ms != 0) {
            for (const auto& kv : prefixed_kvs) {
                redis->pexpire(kv.first, std::chrono::milliseconds(ttl_ms));
            }
        }
        (void)error;
        return true;
    }

    std::size_t size() override {
        auto redis = connect();
        std::size_t count = 0;
        const std::string pattern = cfg_.prefix + ":*";
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
    std::string prefixed(const std::string& key) const {
        return cfg_.prefix + ":" + key;
    }

    // Lazy connect: construction must not block, so the Redis handle is
    // built on first use. Every command call re-resolves the handle.
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

std::unique_ptr<GlobalDataBackend> make_inprocess_data_backend() {
    return std::make_unique<InProcessDataBackend>();
}

std::unique_ptr<GlobalDataBackend> make_redis_data_backend(
    const RedisDataBackendConfig& config) {
    return std::make_unique<RedisDataBackend>(config);
}

}  // namespace shield::global
