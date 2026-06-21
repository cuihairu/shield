// [SHIELD_PLUGIN] shield.cache.v1 interface.
//
// Cache provider. A package providing this interface returns a
// shield_cache_v1* from instance->get_interface("shield.cache.v1").
// The host (or a cache pool) calls connect() once per instance to obtain a
// shield_cache_conn handle bound to the configured backend (e.g. redis).
#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_CACHE_INTERFACE "shield.cache.v1"

struct shield_cache_conn;  // opaque, plugin-defined

struct shield_cache_config {
    const char* host;
    int port;
    const char* password;
    int db;                    // redis DB index, 0 = default
    int connect_timeout_ms;
    int command_timeout_ms;
    int pool_size;
    const char* extra_json;    // backend-specific options
};

struct shield_cache_value {
    int found;
    const char* data;
    int data_len;
    int64_t ttl_remaining_ms;  // -1 = no TTL
};

struct shield_cache_v1 {
    uint32_t struct_size;
    const char* name;          // "redis" | "memory" | ...
    const char* version;

    struct shield_cache_conn* (*connect)(const struct shield_cache_config* cfg,
                                         char* err_buf, int err_buf_size);
    void (*disconnect)(struct shield_cache_conn* conn);
    int  (*ping)(struct shield_cache_conn* conn);

    int (*get)(struct shield_cache_conn* conn, const char* key,
               struct shield_cache_value* out);
    int (*set)(struct shield_cache_conn* conn, const char* key,
               const char* value, int ttl_seconds);
    int (*del)(struct shield_cache_conn* conn, const char* key);
    int (*exists)(struct shield_cache_conn* conn, const char* key);

    int (*incr)(struct shield_cache_conn* conn, const char* key, int64_t* out);
    int (*incr_by)(struct shield_cache_conn* conn, const char* key,
                   int64_t amount, int64_t* out);

    int (*hget)(struct shield_cache_conn* conn, const char* key,
                const char* field, struct shield_cache_value* out);
    int (*hset)(struct shield_cache_conn* conn, const char* key,
                const char* field, const char* value);
    int (*hdel)(struct shield_cache_conn* conn, const char* key,
                const char* field);

    void (*free_value)(struct shield_cache_value* value);
};

#ifdef __cplusplus
}
#endif
