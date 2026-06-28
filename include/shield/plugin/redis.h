// [SHIELD_PLUGIN] shield.redis.v1 interface.
//
// Unified Redis driver. A package providing this interface returns a
// shield_redis_v1* from instance->get_interface("shield.redis.v1").
// The driver holds the redis++ connection pool internally; callers execute
// commands through typed methods (get/set/hset/...) or raw command fallback.
//
// Memory: all methods that return a shield_redis_value_v1* allocate via
// malloc. Callers MUST call free_value() when done. Nested arrays (e.g.
// hgetall) are recursively freed by free_value().
#pragma once

#include "shield/plugin/abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_REDIS_V1 "shield.redis.v1"

// Value type discriminator.
typedef enum shield_redis_type_v1 {
    SHIELD_REDIS_NIL     = 0,
    SHIELD_REDIS_STRING  = 1,
    SHIELD_REDIS_INTEGER = 2,
    SHIELD_REDIS_DOUBLE  = 3,
    SHIELD_REDIS_BOOL    = 4,
    SHIELD_REDIS_ARRAY   = 5,
    SHIELD_REDIS_ERROR   = 6
} shield_redis_type_v1;

// Generic Redis return value. Type field determines which union member is
// valid. For ARRAY, items[item_count] holds nested values (also freed by
// free_value).
typedef struct shield_redis_value_v1 {
    shield_redis_type_v1 type;
    const char* str;                        // STRING / ERROR content
    uint64_t str_len;                       // STRING / ERROR byte length
    int64_t integer;                        // INTEGER
    double number;                          // DOUBLE
    int boolean;                            // BOOL (0 or 1)
    struct shield_redis_value_v1* items;    // ARRAY elements
    uint64_t item_count;                    // ARRAY element count
} shield_redis_value_v1;

// A single argument to command() — raw bytes, not NUL-terminated.
typedef struct shield_redis_arg_v1 {
    const void* data;
    uint64_t len;
} shield_redis_arg_v1;

// A single command inside a pipeline() call.
typedef struct shield_redis_command_v1 {
    const shield_redis_arg_v1* args;
    uint64_t argc;
} shield_redis_command_v1;

// The main interface vtable.
typedef struct shield_redis_v1 {
    uint32_t struct_size;
    const char* interface_name;  // "shield.redis.v1"

    // --- Connection handle ---
    // Returns an opaque handle for use with all methods below. cfg may be
    // NULL (the driver uses its own instance config). err_buf receives an
    // error message on failure. disconnect() releases the handle (the
    // underlying connection pool is managed by the driver instance and is
    // NOT torn down by disconnect).
    void* (*connect)(const void* cfg, char* err_buf, int err_buf_size);
    void  (*disconnect)(void* handle);

    // --- Key-Value ---
    int (*get)(void* inst, const char* key,
               shield_redis_value_v1** out, shield_error_v1* err);
    int (*set)(void* inst, const char* key, const char* val,
               int ttl_sec, shield_error_v1* err);
    int (*del)(void* inst, const char* key, shield_error_v1* err);

    // --- Hash ---
    int (*hget)(void* inst, const char* key, const char* field,
                shield_redis_value_v1** out, shield_error_v1* err);
    int (*hset)(void* inst, const char* key, const char* field,
                const char* val, shield_error_v1* err);
    int (*hgetall)(void* inst, const char* key,
                   shield_redis_value_v1** out, shield_error_v1* err);

    // --- Sorted Set ---
    int (*zadd)(void* inst, const char* key, double score,
                const char* member, shield_error_v1* err);
    int (*zrange)(void* inst, const char* key, int start, int stop,
                  shield_redis_value_v1** out, shield_error_v1* err);

    // --- Pipeline ---
    // Execute multiple commands in a single round-trip. out_array receives
    // one shield_redis_value_v1* per command; out_count is set to the number
    // of results. Caller must free_value() each out_array[i] individually,
    // then free(out_array) itself.
    int (*pipeline)(void* inst, const shield_redis_command_v1* cmds,
                    uint64_t count, shield_redis_value_v1** out_array,
                    uint64_t* out_count, shield_error_v1* err);

    // --- Raw command (escape hatch) ---
    // args[0] is the command name, args[1..] are its arguments.
    // Covers all Redis commands not exposed as typed methods.
    int (*command)(void* inst, const shield_redis_arg_v1* args,
                   uint64_t argc, shield_redis_value_v1** out,
                   shield_error_v1* err);

    // --- Memory management ---
    // Release a value returned by any method above. Recursively frees nested
    // arrays. Safe to call with NULL or on an already-freed value.
    void (*free_value)(shield_redis_value_v1* value);
} shield_redis_v1;

#ifdef __cplusplus
}
#endif
