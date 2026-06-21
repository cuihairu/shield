// [SHIELD_PLUGIN] Cache plugin using Redis
//
// Delegates to shield_redis_plugin for storage.
// Provides a simpler cache-oriented API on top of Redis.

#include "shield/plugin/plugin.h"
#include "shield/plugin/cache_plugin.h"
#include "shield/plugin/redis_plugin.h"

#include <cstring>
#include <string>

namespace {

const shield_redis_plugin* g_redis = nullptr;
shield_redis_conn* g_conn = nullptr;

int cache_init(const void* redis_plugin, char* err_buf, int err_buf_size) {
    if (!redis_plugin) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, "redis plugin not provided", err_buf_size - 1);
        }
        return -1;
    }
    g_redis = static_cast<const shield_redis_plugin*>(redis_plugin);
    // Use the global connection from the Redis plugin.
    // In production, this would get a connection from the pool.
    return 0;
}

void cache_shutdown() {
    g_redis = nullptr;
    g_conn = nullptr;
}

int cache_get(const char* key, shield_cache_value* out) {
    if (!g_redis || !g_conn) { out->found = 0; return -1; }
    shield_redis_value rv{};
    int r = g_redis->get(g_conn, key, &rv);
    if (r == 0 && rv.found) {
        out->found = 1;
        out->data = rv.data;
        out->data_len = rv.data_len;
        out->ttl_remaining_ms = -1;
    } else {
        out->found = 0;
        out->data = nullptr;
        out->data_len = 0;
    }
    return r;
}

int cache_set(const char* key, const char* value, int ttl_seconds) {
    if (!g_redis || !g_conn) return -1;
    return g_redis->set(g_conn, key, value, 0, ttl_seconds);
}

int cache_del(const char* key) {
    if (!g_redis || !g_conn) return -1;
    return g_redis->del(g_conn, key);
}

int cache_exists(const char* key) {
    if (!g_redis || !g_conn) return 0;
    return g_redis->exists(g_conn, key);
}

int cache_incr(const char* key, int64_t* out) {
    if (!g_redis || !g_conn) return -1;
    return g_redis->incr(g_conn, key, out);
}

int cache_incr_by(const char* key, int64_t amount, int64_t* out) {
    if (!g_redis || !g_conn) return -1;
    return g_redis->incr_by(g_conn, key, amount, out);
}

int cache_hget(const char* key, const char* field, shield_cache_value* out) {
    if (!g_redis || !g_conn) { out->found = 0; return -1; }
    shield_redis_value rv{};
    int r = g_redis->hget(g_conn, key, field, &rv);
    if (r == 0 && rv.found) {
        out->found = 1;
        out->data = rv.data;
        out->data_len = rv.data_len;
    } else {
        out->found = 0;
    }
    return r;
}

int cache_hset(const char* key, const char* field, const char* value) {
    if (!g_redis || !g_conn) return -1;
    return g_redis->hset(g_conn, key, field, value, 0);
}

int cache_hdel(const char* key, const char* field) {
    if (!g_redis || !g_conn) return -1;
    return g_redis->hdel(g_conn, key, field);
}

void cache_free_value(shield_cache_value* value) {
    if (value && value->data) {
        std::free(const_cast<char*>(value->data));
        value->data = nullptr;
    }
}

const shield_cache_plugin g_cache_plugin = {
    SHIELD_CACHE_ABI_VERSION,
    "redis_cache",
    "1.0.0",

    cache_init,
    cache_shutdown,

    cache_get,
    cache_set,
    cache_del,
    cache_exists,

    cache_incr,
    cache_incr_by,

    cache_hget,
    cache_hset,
    cache_hdel,

    cache_free_value,
};

const shield_plugin g_plugin = {
    SHIELD_PLUGIN_ABI_VERSION,
    SHIELD_PLUGIN_TYPE_CACHE,
    "shield_cache",
    "1.0.0",
    "Cache plugin using Redis",
    "Shield",

    [](const shield_host_t host, const shield_host_api* api,
       const shield_plugin_config*, char* err, int err_len) -> int {
        const shield_plugin* redis_p = api->find_plugin(host, "shield_redis");
        if (!redis_p) {
            if (err && err_len > 0) std::strncpy(err, "shield_redis not loaded", err_len - 1);
            return -1;
        }
        return cache_init(redis_p->vtable, err, err_len);
    },

    []() { cache_shutdown(); },
    []() -> int { return 1; },
    [](int) -> const shield_plugin_capability* {
        static shield_plugin_capability cap = {"cache", "1.0.0", "Redis-backed cache"};
        return &cap;
    },

    &g_cache_plugin,
};

}  // namespace

extern "C" __declspec(dllexport)
const struct shield_plugin* shield_plugin_api(void) {
    return &g_plugin;
}
