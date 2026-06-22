// [SHIELD_PLUGIN] cache.redis — Redis-backed provider for shield.cache.v1.
//
// v1 ABI (shield_plugin_get_v1). Holds its own sw::redis::Redis instance per
// shield_cache_conn — there is no shared global "redis plugin" anymore (the
// legacy plugin system had one; v1 does not expose redis as a public iface).
// Each cache instance connects independently; if multiple instances point at
// the same Redis they will share data via Redis itself, not via the host.
//
// Mapping the legacy API to v1: redis_get/set/del/exists/incr/incr_by/hget/
// hset/hdel map 1:1. `decr` and `hgetall` from the old redis surface are not
// part of shield.cache.v1 and are dropped. TTL is exposed through the v1
// `set(key, value, ttl_seconds)` signature.

#include "shield/plugin/abi.h"
#include "shield/plugin/cache.h"

#include <sw/redis++/redis++.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {

char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

// shield_cache_conn is opaque in cache.h; concrete layout lives here.
struct shield_cache_conn {
    std::shared_ptr<sw::redis::Redis> redis;
};

void fill_value(shield_cache_value* out, const std::optional<std::string>& v) {
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

// ---------------------------------------------------------------------------
// v1 cache vtable
// ---------------------------------------------------------------------------
const shield_cache_v1& cache_vtable() {
    static const shield_cache_v1 v = {
        sizeof(shield_cache_v1),
        "redis",
        "1.0.0",
        // connect
        [](const shield_cache_config* cfg,
           char* err_buf, int err_buf_size) -> shield_cache_conn* {
            if (!cfg) return nullptr;
            try {
                sw::redis::ConnectionOptions opts;
                opts.host = cfg->host && cfg->host[0] ? cfg->host : "localhost";
                opts.port = cfg->port > 0 ? cfg->port : 6379;
                if (cfg->password && cfg->password[0])
                    opts.password = cfg->password;
                opts.db = cfg->db > 0 ? cfg->db : 0;
                opts.connect_timeout = std::chrono::milliseconds(
                    cfg->connect_timeout_ms > 0 ? cfg->connect_timeout_ms : 5000);
                opts.socket_timeout = std::chrono::milliseconds(
                    cfg->command_timeout_ms > 0 ? cfg->command_timeout_ms : 5000);

                sw::redis::ConnectionPoolOptions pool_opts;
                pool_opts.size = cfg->pool_size > 0
                                     ? static_cast<std::size_t>(cfg->pool_size)
                                     : 8;

                auto redis = std::make_shared<sw::redis::Redis>(
                    sw::redis::Redis(opts, pool_opts));
                redis->ping();
                return new shield_cache_conn{redis};
            } catch (const std::exception& e) {
                if (err_buf && err_buf_size > 0) {
                    std::snprintf(err_buf, err_buf_size, "%s", e.what());
                }
                return nullptr;
            }
        },
        // disconnect
        [](shield_cache_conn* c) {
            delete c;
        },
        // ping
        [](shield_cache_conn* c) -> int {
            if (!c || !c->redis) return 0;
            try { c->redis->ping(); return 1; }
            catch (...) { return 0; }
        },
        // get
        [](shield_cache_conn* c, const char* key,
           shield_cache_value* out) -> int {
            if (!c || !c->redis || !key) {
                out->found = 0; out->data = nullptr; out->data_len = 0;
                return -1;
            }
            try {
                auto v = c->redis->get(std::string(key));
                fill_value(out, v);
                return 0;
            } catch (...) {
                out->found = 0; out->data = nullptr; out->data_len = 0;
                return -1;
            }
        },
        // set
        [](shield_cache_conn* c, const char* key,
           const char* value, int ttl_seconds) -> int {
            if (!c || !c->redis || !key || !value) return -1;
            try {
                if (ttl_seconds > 0) {
                    c->redis->set(std::string(key), std::string(value),
                                  std::chrono::seconds(ttl_seconds));
                } else {
                    c->redis->set(std::string(key), std::string(value));
                }
                return 0;
            } catch (...) { return -1; }
        },
        // del
        [](shield_cache_conn* c, const char* key) -> int {
            if (!c || !c->redis || !key) return -1;
            try { c->redis->del(std::string(key)); return 0; }
            catch (...) { return -1; }
        },
        // exists
        [](shield_cache_conn* c, const char* key) -> int {
            if (!c || !c->redis || !key) return 0;
            try { return c->redis->exists(std::string(key)) > 0 ? 1 : 0; }
            catch (...) { return 0; }
        },
        // incr
        [](shield_cache_conn* c, const char* key, int64_t* out) -> int {
            if (!c || !c->redis || !key || !out) return -1;
            try { *out = c->redis->incr(std::string(key)); return 0; }
            catch (...) { return -1; }
        },
        // incr_by
        [](shield_cache_conn* c, const char* key,
           int64_t amount, int64_t* out) -> int {
            if (!c || !c->redis || !key || !out) return -1;
            try { *out = c->redis->incrby(std::string(key), amount); return 0; }
            catch (...) { return -1; }
        },
        // hget
        [](shield_cache_conn* c, const char* key,
           const char* field, shield_cache_value* out) -> int {
            if (!c || !c->redis || !key || !field) {
                out->found = 0; out->data = nullptr; out->data_len = 0;
                return -1;
            }
            try {
                auto v = c->redis->hget(std::string(key), std::string(field));
                fill_value(out, v);
                return 0;
            } catch (...) {
                out->found = 0; out->data = nullptr; out->data_len = 0;
                return -1;
            }
        },
        // hset
        [](shield_cache_conn* c, const char* key,
           const char* field, const char* value) -> int {
            if (!c || !c->redis || !key || !field || !value) return -1;
            try {
                c->redis->hset(std::string(key), std::string(field),
                               std::string(value));
                return 0;
            } catch (...) { return -1; }
        },
        // hdel
        [](shield_cache_conn* c, const char* key, const char* field) -> int {
            if (!c || !c->redis || !key || !field) return -1;
            try {
                c->redis->hdel(std::string(key), std::string(field));
                return 0;
            } catch (...) { return -1; }
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

}  // namespace

// ---------------------------------------------------------------------------
// v1 ABI entry
// ---------------------------------------------------------------------------
namespace {

struct cache_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
};

int cache_create(const shield_plugin_create_args_v1* args,
                 shield_plugin_instance_v1** out,
                 shield_error_v1* err) {
    (void)err;
    auto* inst = new cache_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1*,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_CACHE_INTERFACE) == 0)
            return &cache_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1*, shield_error_v1*) { return 0; };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        delete reinterpret_cast<cache_instance*>(self);
    };
    *out = &inst->shell;
    return 0;
}

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT
const struct shield_plugin_abi_v1* shield_plugin_get_v1(void) {
    static const struct shield_plugin_abi_v1 abi = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "cache.redis",
        "1.0.0",
        cache_create,
    };
    return &abi;
}
