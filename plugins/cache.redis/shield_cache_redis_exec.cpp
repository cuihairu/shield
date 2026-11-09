// [SHIELD_PLUGIN] cache.redis — C-ABI executor: the opaque conn
// (global scope for lambda-to-fnptr conversion), the value-tree
// fill helpers, the shared open_redis connection builder and the
// shield.cache.v1 surface (get/set/del/ttl/incr/hash) over
// redis++ or the redis.driver vtable.
// Split out of shield_cache_redis.cpp.
#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/cache.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/pool_stats.h"
#include "shield/plugin/redis.h"
#include "shield_cache_redis_impl.hpp"
#include "shield_lua_plugin_binding.hpp"

// shield_cache_conn is opaque in cache.h; concrete layout lives here.
// When redis_driver is set, all operations go through redis.driver's vtable.
// Otherwise, the conn uses its own sw::redis::Redis connection pool.
// NOTE: defined at global scope (not inside anonymous namespace) so that
// lambda-to-function-pointer conversion sees the same type as the vtable
// declared in cache.h.
struct shield_cache_conn {
    std::shared_ptr<sw::redis::Redis> redis;        // fallback: own connection
    const shield_redis_v2* redis_driver = nullptr;  // preferred: redis.driver
    void* redis_handle = nullptr;                   // opaque handle from driver
};

namespace {
char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}
}  // namespace

namespace {
void fill_value(shield_cache_value* out, const sw::redis::OptionalString& v) {
    if (v) {
        out->found = 1;
        out->data = dup_string(v->c_str());
        out->data_len = static_cast<int>(v->size());
        out->ttl_remaining_ms = -1;
    } else {
        out->found = 0;
        out->data = nullptr;
        out->data_len = 0;
        out->ttl_remaining_ms = -1;
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// v1 cache vtable
//
// open_redis() builds a shared sw::redis::Redis from the same config source
// (shield_cache_config for C callers, cache_instance for Lua callers). Both
// paths share the connection/pool logic.
// ---------------------------------------------------------------------------

std::shared_ptr<sw::redis::Redis> open_redis(const std::string& host, int port,
                                             const std::string& password,
                                             int db, int pool_size,
                                             int connect_timeout_ms,
                                             int command_timeout_ms,
                                             std::string* err) {
    try {
        sw::redis::ConnectionOptions opts;
        opts.host = host.empty() ? "localhost" : host;
        opts.port = port > 0 ? port : 6379;
        if (!password.empty()) opts.password = password;
        opts.db = db > 0 ? db : 0;
        opts.connect_timeout = std::chrono::milliseconds(
            connect_timeout_ms > 0 ? connect_timeout_ms : 5000);
        opts.socket_timeout = std::chrono::milliseconds(
            command_timeout_ms > 0 ? command_timeout_ms : 5000);

        sw::redis::ConnectionPoolOptions pool_opts;
        pool_opts.size =
            pool_size > 0 ? static_cast<std::size_t>(pool_size) : 8;

        auto redis = std::make_shared<sw::redis::Redis>(
            sw::redis::Redis(opts, pool_opts));
        redis->ping();
        return redis;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return nullptr;
    }
}

const shield_cache_v1& cache_vtable() {
    static const shield_cache_v1 v = {
        sizeof(shield_cache_v1),
        SHIELD_CACHE_INTERFACE,
        "redis",
        "1.0.0",
        // connect
        [](const shield_cache_config* cfg, char* err_buf,
           int err_buf_size) -> shield_cache_conn* {
            // NOTE: This vtable is process-wide (static). The redis_driver
            // pointer is injected per-instance, so we cannot access it here.
            // The per-instance connect path (used by Lua and C++ callers)
            // goes through cache_instance::connect_instance() instead.
            // This legacy path is kept for direct C vtable callers who pass
            // their own config.
            if (!cfg) return nullptr;
            std::string err;
            std::string password = cfg->password ? cfg->password : "";
            std::string host =
                cfg->host && cfg->host[0] ? cfg->host : "localhost";
            auto redis = open_redis(host, cfg->port, password, cfg->db,
                                    cfg->pool_size, cfg->connect_timeout_ms,
                                    cfg->command_timeout_ms, &err);
            if (!redis) {
                if (err_buf && err_buf_size > 0) {
                    std::snprintf(err_buf, err_buf_size, "%s", err.c_str());
                }
                return nullptr;
            }
            return new shield_cache_conn{redis, nullptr, nullptr};
        },
        // disconnect
        [](shield_cache_conn* c) { delete c; },
        // ping
        [](shield_cache_conn* c) -> int {
            if (!c || !c->redis) return 0;
            try {
                c->redis->ping();
                return 1;
            } catch (...) {
                return 0;
            }
        },
        // get
        [](shield_cache_conn* c, const char* key,
           shield_cache_value* out) -> int {
            if (!c || !c->redis || !key) {
                out->found = 0;
                out->data = nullptr;
                out->data_len = 0;
                return -1;
            }
            try {
                auto v = c->redis->get(std::string(key));
                fill_value(out, v);
                return 0;
            } catch (...) {
                out->found = 0;
                out->data = nullptr;
                out->data_len = 0;
                return -1;
            }
        },
        // set
        [](shield_cache_conn* c, const char* key, const char* value,
           int ttl_seconds) -> int {
            if (!c || !c->redis || !key || !value) return -1;
            try {
                if (ttl_seconds > 0) {
                    c->redis->set(std::string(key), std::string(value),
                                  std::chrono::seconds(ttl_seconds));
                } else {
                    c->redis->set(std::string(key), std::string(value));
                }
                return 0;
            } catch (...) {
                return -1;
            }
        },
        // del
        [](shield_cache_conn* c, const char* key) -> int {
            if (!c || !c->redis || !key) return -1;
            try {
                c->redis->del(std::string(key));
                return 0;
            } catch (...) {
                return -1;
            }
        },
        // exists
        [](shield_cache_conn* c, const char* key) -> int {
            if (!c || !c->redis || !key) return 0;
            try {
                return c->redis->exists(std::string(key)) > 0 ? 1 : 0;
            } catch (...) {
                return 0;
            }
        },
        // incr
        [](shield_cache_conn* c, const char* key, int64_t* out) -> int {
            if (!c || !c->redis || !key || !out) return -1;
            try {
                *out = c->redis->incr(std::string(key));
                return 0;
            } catch (...) {
                return -1;
            }
        },
        // incr_by
        [](shield_cache_conn* c, const char* key, int64_t amount,
           int64_t* out) -> int {
            if (!c || !c->redis || !key || !out) return -1;
            try {
                *out = c->redis->incrby(std::string(key), amount);
                return 0;
            } catch (...) {
                return -1;
            }
        },
        // hget
        [](shield_cache_conn* c, const char* key, const char* field,
           shield_cache_value* out) -> int {
            if (!c || !c->redis || !key || !field) {
                out->found = 0;
                out->data = nullptr;
                out->data_len = 0;
                return -1;
            }
            try {
                auto v = c->redis->hget(std::string(key), std::string(field));
                fill_value(out, v);
                return 0;
            } catch (...) {
                out->found = 0;
                out->data = nullptr;
                out->data_len = 0;
                return -1;
            }
        },
        // hset
        [](shield_cache_conn* c, const char* key, const char* field,
           const char* value) -> int {
            if (!c || !c->redis || !key || !field || !value) return -1;
            try {
                c->redis->hset(std::string(key), std::string(field),
                               std::string(value));
                return 0;
            } catch (...) {
                return -1;
            }
        },
        // hdel
        [](shield_cache_conn* c, const char* key, const char* field) -> int {
            if (!c || !c->redis || !key || !field) return -1;
            try {
                c->redis->hdel(std::string(key), std::string(field));
                return 0;
            } catch (...) {
                return -1;
            }
        },
        // free_value
        [](shield_cache_value* val) {
            if (val && val->data) {
                std::free(const_cast<char*>(val->data));
                val->data = nullptr;
                val->data_len = 0;
            }
        },
    };
    return v;
}
