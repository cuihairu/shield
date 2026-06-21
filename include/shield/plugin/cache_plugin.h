// [SHIELD_PLUGIN] Cache backend plugin C ABI
//
// Stable C interface for cache backends (Redis, Memcached, in-memory, etc.).
// Each cache plugin DLL exports shield_cache_plugin_api() returning a
// pointer to a static shield_cache_plugin struct.
//
// Integration with shield_plugin system:
//   type = SHIELD_PLUGIN_TYPE_CACHE, vtable → shield_cache_plugin*

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_CACHE_ABI_VERSION 1

struct shield_cache_conn;

struct shield_cache_config {
    const char* host;
    int port;
    const char* password;
    int db;                        // Redis DB index, 0 = default
    int connect_timeout_ms;
    int command_timeout_ms;
    int pool_size;
    const char* extra_json;        // driver-specific options
};

struct shield_cache_value {
    int found;                     // 1 = key exists, 0 = not found
    const char* data;              // NULL-terminated string value
    int64_t ttl_remaining_ms;      // -1 = no TTL, 0 = expired
};

struct shield_cache_plugin {
    uint32_t abi_version;
    const char* name;              // "redis", "memcached", "memory"
    const char* version;

    // Connection
    struct shield_cache_conn* (*connect)(const struct shield_cache_config* config,
                                         char* err_buf, int err_buf_size);
    void (*disconnect)(struct shield_cache_conn* conn);
    int (*ping)(struct shield_cache_conn* conn);

    // Key-value operations
    int (*get)(struct shield_cache_conn* conn, const char* key,
               struct shield_cache_value* out);
    int (*set)(struct shield_cache_conn* conn, const char* key,
               const char* value, int ttl_seconds);
    int (*del)(struct shield_cache_conn* conn, const char* key);
    int (*exists)(struct shield_cache_conn* conn, const char* key);

    // Atomic operations
    int (*incr)(struct shield_cache_conn* conn, const char* key, int64_t* out);
    int (*decr)(struct shield_cache_conn* conn, const char* key, int64_t* out);

    // Hash operations
    int (*hget)(struct shield_cache_conn* conn, const char* key,
                const char* field, struct shield_cache_value* out);
    int (*hset)(struct shield_cache_conn* conn, const char* key,
                const char* field, const char* value);
    int (*hdel)(struct shield_cache_conn* conn, const char* key,
                const char* field);

    // Memory
    void (*free_value)(struct shield_cache_value* value);
};

// Entry point exported by every cache plugin DLL.
#ifdef _WIN32
#define SHIELD_CACHE_EXPORT __declspec(dllexport)
#else
#define SHIELD_CACHE_EXPORT __attribute__((visibility("default")))
#endif

SHIELD_CACHE_EXPORT
const struct shield_cache_plugin* shield_cache_plugin_api(void);

#ifdef __cplusplus
}
#endif
