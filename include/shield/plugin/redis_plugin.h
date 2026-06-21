// [SHIELD_PLUGIN] Redis plugin C ABI
//
// Redis is a shared infrastructure plugin. Other plugins (CACHE, QUEUE,
// LEADERBOARD) depend on it via host_api->find_plugin("redis").
//
// This plugin provides:
// - Connection pool management
// - Key-value operations (GET/SET/DEL/EXISTS)
// - Atomic operations (INCR/DECR)
// - Hash operations (HGET/HSET/HDEL)
// - Sorted set operations (ZADD/ZRANGE/ZRANK/ZREM) — for leaderboards
// - Pub/Sub (PUBLISH/SUBSCRIBE/UNSUBSCRIBE) — for message queue
// - TTL management
//
// Other plugins use this as a shared dependency:
//   const shield_plugin* p = host_api->find_plugin(host, "shield_redis");
//   const shield_redis_plugin* redis = p->vtable;
//   redis->get(conn, "key", &out);

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_REDIS_ABI_VERSION 1

struct shield_redis_conn;

struct shield_redis_config {
    const char* host;
    int port;
    const char* password;
    int db;                        // Redis DB index, 0 = default
    int connect_timeout_ms;
    int command_timeout_ms;
    int pool_size;                 // connection pool size
    const char* extra_json;        // driver-specific options
};

// Generic string value result.
struct shield_redis_value {
    int found;                     // 1 = exists, 0 = not found
    const char* data;
    int data_len;
};

// Sorted set entry (for leaderboard).
struct shield_redis_zentry {
    const char* member;
    double score;
};

// Pub/Sub message callback.
typedef void (*shield_redis_on_message)(
    const char* channel,
    const char* data,
    int data_len,
    void* user_data);

struct shield_redis_plugin {
    uint32_t abi_version;
    const char* name;              // "redis", "valkey", "dragonfly"
    const char* version;

    // --- Connection Pool ------------------------------------------------
    struct shield_redis_conn* (*connect)(const struct shield_redis_config* config,
                                         char* err_buf, int err_buf_size);
    void (*disconnect)(struct shield_redis_conn* conn);
    int (*ping)(struct shield_redis_conn* conn);

    // --- Key-Value Operations -------------------------------------------
    int (*get)(struct shield_redis_conn* conn, const char* key,
               struct shield_redis_value* out);
    int (*set)(struct shield_redis_conn* conn, const char* key,
               const char* value, int value_len, int ttl_seconds);
    int (*del)(struct shield_redis_conn* conn, const char* key);
    int (*exists)(struct shield_redis_conn* conn, const char* key);

    // --- Atomic Operations ----------------------------------------------
    int (*incr)(struct shield_redis_conn* conn, const char* key, int64_t* out);
    int (*decr)(struct shield_redis_conn* conn, const char* key, int64_t* out);
    int (*incr_by)(struct shield_redis_conn* conn, const char* key,
                   int64_t amount, int64_t* out);

    // --- Hash Operations ------------------------------------------------
    int (*hget)(struct shield_redis_conn* conn, const char* key,
                const char* field, struct shield_redis_value* out);
    int (*hset)(struct shield_redis_conn* conn, const char* key,
                const char* field, const char* value, int value_len);
    int (*hdel)(struct shield_redis_conn* conn, const char* key,
                const char* field);
    int (*hgetall)(struct shield_redis_conn* conn, const char* key,
                   struct shield_redis_value** out_keys,
                   struct shield_redis_value** out_values,
                   int* out_count);

    // --- Sorted Set Operations (for Leaderboard) ------------------------
    int (*zadd)(struct shield_redis_conn* conn, const char* key,
                double score, const char* member);
    int (*zrem)(struct shield_redis_conn* conn, const char* key,
                const char* member);
    int (*zscore)(struct shield_redis_conn* conn, const char* key,
                  const char* member, double* out_score);
    int (*zrank)(struct shield_redis_conn* conn, const char* key,
                 const char* member, int64_t* out_rank);
    int (*zrange)(struct shield_redis_conn* conn, const char* key,
                  int64_t start, int64_t stop,
                  struct shield_redis_zentry** out_entries, int* out_count);
    int (*zrevrange)(struct shield_redis_conn* conn, const char* key,
                     int64_t start, int64_t stop,
                     struct shield_redis_zentry** out_entries, int* out_count);
    int (*zrangebyscore)(struct shield_redis_conn* conn, const char* key,
                         double min_score, double max_score,
                         struct shield_redis_zentry** out_entries, int* out_count);
    int (*zcount)(struct shield_redis_conn* conn, const char* key,
                  int64_t* out_count);

    // --- Pub/Sub Operations (for Message Queue) -------------------------
    int (*publish)(struct shield_redis_conn* conn, const char* channel,
                   const char* data, int data_len);
    int (*subscribe)(struct shield_redis_conn* conn, const char* channel,
                     shield_redis_on_message callback, void* user_data);
    int (*unsubscribe)(struct shield_redis_conn* conn, const char* channel);

    // --- Composite Score Encoding (for multi-field leaderboard) ----------
    //
    // Encode multiple fields into a single double for ZSET storage.
    // fields: array of field values (must be non-negative integers).
    // bits: array of bit widths for each field.
    // Returns the composite score.
    //
    // Example: score(0-999999) + level(0-999) + time_rank(0-999999)
    //   double fields[] = {1500, 45, 12345};
    //   int bits[] = {20, 10, 20};
    //   double composite = encode_composite(fields, bits, 3);
    //   // composite naturally sorts: score DESC, level DESC, time ASC
    double (*encode_composite)(const double* fields, const int* bits, int count);

    // Decode a composite score back into individual fields.
    int (*decode_composite)(double composite, const int* bits, int count,
                            double* out_fields);

    // --- Memory ---------------------------------------------------------
    void (*free_value)(struct shield_redis_value* value);
    void (*free_zentries)(struct shield_redis_zentry* entries, int count);
};

// Entry point exported by every Redis plugin DLL.
#ifdef _WIN32
#define SHIELD_REDIS_EXPORT __declspec(dllexport)
#else
#define SHIELD_REDIS_EXPORT __attribute__((visibility("default")))
#endif

SHIELD_REDIS_EXPORT
const struct shield_redis_plugin* shield_redis_plugin_api(void);

#ifdef __cplusplus
}
#endif
