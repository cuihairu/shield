// [SHIELD_PLUGIN] Cache plugin C ABI
//
// High-level cache abstraction. Depends on a Redis plugin for storage.
// Plugins that need caching call cache->get/set instead of raw Redis.
//
// Usage from another plugin:
//   const shield_plugin* p = host_api->find_plugin(host, "shield_cache");
//   const shield_cache_plugin* cache = p->vtable;
//   cache->get("player:123", &out);
//
// The cache plugin internally uses the Redis plugin:
//   const shield_plugin* rp = host_api->find_plugin(host, "shield_redis");
//   const shield_redis_plugin* redis = rp->vtable;

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_CACHE_ABI_VERSION 1

struct shield_cache_value {
    int found;
    const char* data;
    int data_len;
    int64_t ttl_remaining_ms;      // -1 = no TTL
};

struct shield_cache_plugin {
    uint32_t abi_version;
    const char* name;
    const char* version;

    // Initialize with a reference to the Redis plugin.
    int (*init)(const void* redis_plugin, char* err_buf, int err_buf_size);
    void (*shutdown)(void);

    // Key-value operations (delegate to Redis).
    int (*get)(const char* key, struct shield_cache_value* out);
    int (*set)(const char* key, const char* value, int ttl_seconds);
    int (*del)(const char* key);
    int (*exists)(const char* key);

    // Atomic operations.
    int (*incr)(const char* key, int64_t* out);
    int (*incr_by)(const char* key, int64_t amount, int64_t* out);

    // Hash operations.
    int (*hget)(const char* key, const char* field, struct shield_cache_value* out);
    int (*hset)(const char* key, const char* field, const char* value);
    int (*hdel)(const char* key, const char* field);

    // Memory
    void (*free_value)(struct shield_cache_value* value);
};

#ifdef __cplusplus
}
#endif
