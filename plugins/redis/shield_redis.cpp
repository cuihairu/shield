// [SHIELD_PLUGIN_REDIS] Redis plugin implementation using redis++
//
// Wraps redis++ into the shield_redis_plugin C ABI interface.
// Other plugins (CACHE, QUEUE, LEADERBOARD) depend on this.

#include "shield/plugin/plugin.h"
#include "shield/plugin/redis_plugin.h"

#include <sw/redis++/redis++.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

void fill_value(shield_redis_value* out, const std::string& data) {
    out->found = 1;
    out->data = dup_string(data.c_str());
    out->data_len = static_cast<int>(data.size());
}

}  // namespace

// Opaque connection handle.
struct shield_redis_conn {
    std::shared_ptr<sw::redis::Redis> redis;
};

namespace {

std::shared_ptr<sw::redis::Redis> g_redis;
shield_redis_conn g_conn{g_redis};

// --- Plugin vtable functions ---------------------------------------------

shield_redis_conn* redis_connect(const shield_redis_config* config,
                                 char* err_buf, int err_buf_size) {
    try {
        sw::redis::ConnectionOptions opts;
        opts.host = config->host ? config->host : "localhost";
        opts.port = config->port > 0 ? config->port : 6379;
        if (config->password && config->password[0]) {
            opts.password = config->password;
        }
        opts.db = config->db;
        opts.connect_timeout = std::chrono::milliseconds(
            config->connect_timeout_ms > 0 ? config->connect_timeout_ms : 5000);
        opts.socket_timeout = std::chrono::milliseconds(
            config->command_timeout_ms > 0 ? config->command_timeout_ms : 5000);

        g_redis = std::make_shared<sw::redis::Redis>(opts);
        g_redis->ping();
        g_conn.redis = g_redis;
        return &g_conn;
    } catch (const std::exception& e) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, e.what(), err_buf_size - 1);
            err_buf[err_buf_size - 1] = '\0';
        }
        return nullptr;
    }
}

void redis_disconnect(shield_redis_conn* conn) {
    (void)conn;
}

int redis_ping(shield_redis_conn* conn) {
    try {
        conn->redis->ping();
        return 1;
    } catch (...) {
        return 0;
    }
}

int redis_get(shield_redis_conn* conn, const char* key,
              shield_redis_value* out) {
    try {
        auto val = conn->redis->get(std::string(key));
        if (val) fill_value(out, *val);
        else { out->found = 0; out->data = nullptr; out->data_len = 0; }
        return 0;
    } catch (...) { return -1; }
}

int redis_set(shield_redis_conn* conn, const char* key,
              const char* value, int value_len, int ttl_seconds) {
    try {
        std::string val(value, value_len > 0 ? value_len : std::strlen(value));
        if (ttl_seconds > 0)
            conn->redis->set(std::string(key), val, std::chrono::seconds(ttl_seconds));
        else
            conn->redis->set(std::string(key), val);
        return 0;
    } catch (...) { return -1; }
}

int redis_del(shield_redis_conn* conn, const char* key) {
    try { conn->redis->del(std::string(key)); return 0; }
    catch (...) { return -1; }
}

int redis_exists(shield_redis_conn* conn, const char* key) {
    try { return conn->redis->exists(std::string(key)) > 0 ? 1 : 0; }
    catch (...) { return 0; }
}

int redis_incr(shield_redis_conn* conn, const char* key, int64_t* out) {
    try { *out = conn->redis->incr(std::string(key)); return 0; }
    catch (...) { return -1; }
}

int redis_decr(shield_redis_conn* conn, const char* key, int64_t* out) {
    try { *out = conn->redis->decr(std::string(key)); return 0; }
    catch (...) { return -1; }
}

int redis_incr_by(shield_redis_conn* conn, const char* key,
                  int64_t amount, int64_t* out) {
    try { *out = conn->redis->incrby(std::string(key), amount); return 0; }
    catch (...) { return -1; }
}

int redis_hget(shield_redis_conn* conn, const char* key,
               const char* field, shield_redis_value* out) {
    try {
        auto val = conn->redis->hget(std::string(key), std::string(field));
        if (val) fill_value(out, *val);
        else { out->found = 0; out->data = nullptr; out->data_len = 0; }
        return 0;
    } catch (...) { return -1; }
}

int redis_hset(shield_redis_conn* conn, const char* key,
               const char* field, const char* value, int value_len) {
    try {
        std::string val(value, value_len > 0 ? value_len : std::strlen(value));
        conn->redis->hset(std::string(key), std::string(field), val);
        return 0;
    } catch (...) { return -1; }
}

int redis_hdel(shield_redis_conn* conn, const char* key, const char* field) {
    try { conn->redis->hdel(std::string(key), std::string(field)); return 0; }
    catch (...) { return -1; }
}

int redis_hgetall(shield_redis_conn* conn, const char* key,
                  shield_redis_value** out_keys, shield_redis_value** out_values,
                  int* out_count) {
    try {
        std::unordered_map<std::string, std::string> result;
        conn->redis->hgetall(std::string(key), std::inserter(result, result.end()));
        *out_count = static_cast<int>(result.size());
        if (result.empty()) { *out_keys = nullptr; *out_values = nullptr; return 0; }
        *out_keys = static_cast<shield_redis_value*>(
            std::calloc(result.size(), sizeof(shield_redis_value)));
        *out_values = static_cast<shield_redis_value*>(
            std::calloc(result.size(), sizeof(shield_redis_value)));
        int i = 0;
        for (const auto& [k, v] : result) {
            (*out_keys)[i] = {1, dup_string(k.c_str()), static_cast<int>(k.size())};
            (*out_values)[i] = {1, dup_string(v.c_str()), static_cast<int>(v.size())};
            ++i;
        }
        return 0;
    } catch (...) { *out_count = 0; return -1; }
}

int redis_zadd(shield_redis_conn* conn, const char* key,
               double score, const char* member) {
    try { conn->redis->zadd(std::string(key), std::string(member), score); return 0; }
    catch (...) { return -1; }
}

int redis_zrem(shield_redis_conn* conn, const char* key, const char* member) {
    try { conn->redis->zrem(std::string(key), std::string(member)); return 0; }
    catch (...) { return -1; }
}

int redis_zscore(shield_redis_conn* conn, const char* key,
                 const char* member, double* out_score) {
    try {
        auto s = conn->redis->zscore(std::string(key), std::string(member));
        *out_score = s ? *s : 0.0;
        return s ? 0 : -1;
    } catch (...) { *out_score = 0; return -1; }
}

int redis_zrank(shield_redis_conn* conn, const char* key,
                const char* member, int64_t* out_rank) {
    try {
        auto r = conn->redis->zrank(std::string(key), std::string(member));
        *out_rank = r ? static_cast<int64_t>(*r) + 1 : 0;
        return r ? 0 : -1;
    } catch (...) { *out_rank = 0; return -1; }
}

int redis_zrange(shield_redis_conn* conn, const char* key,
                 int64_t start, int64_t stop,
                 shield_redis_zentry** out_entries, int* out_count) {
    try {
        std::vector<std::pair<std::string, double>> result;
        conn->redis->zrange(std::string(key), start, stop, std::back_inserter(result));
        *out_count = static_cast<int>(result.size());
        if (result.empty()) { *out_entries = nullptr; return 0; }
        *out_entries = static_cast<shield_redis_zentry*>(
            std::calloc(result.size(), sizeof(shield_redis_zentry)));
        for (size_t i = 0; i < result.size(); ++i) {
            (*out_entries)[i].member = dup_string(result[i].first.c_str());
            (*out_entries)[i].score = result[i].second;
        }
        return 0;
    } catch (...) { *out_count = 0; return -1; }
}

int redis_zrevrange(shield_redis_conn* conn, const char* key,
                    int64_t start, int64_t stop,
                    shield_redis_zentry** out_entries, int* out_count) {
    try {
        std::vector<std::pair<std::string, double>> result;
        conn->redis->zrevrange(std::string(key), start, stop, std::back_inserter(result));
        *out_count = static_cast<int>(result.size());
        if (result.empty()) { *out_entries = nullptr; return 0; }
        *out_entries = static_cast<shield_redis_zentry*>(
            std::calloc(result.size(), sizeof(shield_redis_zentry)));
        for (size_t i = 0; i < result.size(); ++i) {
            (*out_entries)[i].member = dup_string(result[i].first.c_str());
            (*out_entries)[i].score = result[i].second;
        }
        return 0;
    } catch (...) { *out_count = 0; return -1; }
}

int redis_zrangebyscore(shield_redis_conn* conn, const char* key,
                        double min_score, double max_score,
                        shield_redis_zentry** out_entries, int* out_count) {
    // Use zrange with BYSCORE option (redis++ API varies by version).
    // Fall back to zrange if zrangebyscore is not available.
    try {
        std::vector<std::pair<std::string, double>> result;
        // Try the generic zrange with score bounds.
        conn->redis->zrange(std::string(key), min_score, max_score,
                            std::back_inserter(result));
        *out_count = static_cast<int>(result.size());
        if (result.empty()) { *out_entries = nullptr; return 0; }
        *out_entries = static_cast<shield_redis_zentry*>(
            std::calloc(result.size(), sizeof(shield_redis_zentry)));
        for (size_t i = 0; i < result.size(); ++i) {
            (*out_entries)[i].member = dup_string(result[i].first.c_str());
            (*out_entries)[i].score = result[i].second;
        }
        return 0;
    } catch (...) { *out_count = 0; return -1; }
}

int redis_zcount(shield_redis_conn* conn, const char* key, int64_t* out_count) {
    try {
        *out_count = static_cast<int64_t>(conn->redis->zcard(std::string(key)));
        return 0;
    } catch (...) { *out_count = 0; return -1; }
}

int redis_publish(shield_redis_conn* conn, const char* channel,
                  const char* data, int data_len) {
    try {
        std::string msg(data, data_len > 0 ? data_len : std::strlen(data));
        return static_cast<int>(conn->redis->publish(std::string(channel), msg));
    } catch (...) { return -1; }
}

// Subscribe callback storage (for future pub/sub integration).
struct SubEntry { shield_redis_on_message cb; void* ud; };
std::unordered_map<std::string, SubEntry> g_subs;
std::mutex g_sub_mutex;

int redis_subscribe(shield_redis_conn* conn, const char* channel,
                    shield_redis_on_message callback, void* user_data) {
    std::lock_guard<std::mutex> lock(g_sub_mutex);
    g_subs[std::string(channel)] = {callback, user_data};
    return 0;
}

int redis_unsubscribe(shield_redis_conn* conn, const char* channel) {
    std::lock_guard<std::mutex> lock(g_sub_mutex);
    g_subs.erase(std::string(channel));
    return 0;
}

double redis_encode_composite(const double* fields, const int* bits, int count) {
    uint64_t composite = 0;
    int total_bits = 0;
    for (int i = 0; i < count; ++i) total_bits += bits[i];
    int shift = total_bits;
    for (int i = 0; i < count; ++i) {
        shift -= bits[i];
        uint64_t mask = (1ULL << bits[i]) - 1;
        composite |= (static_cast<uint64_t>(fields[i]) & mask) << shift;
    }
    return static_cast<double>(composite);
}

int redis_decode_composite(double composite, const int* bits, int count,
                           double* out_fields) {
    uint64_t val = static_cast<uint64_t>(composite);
    int total_bits = 0;
    for (int i = 0; i < count; ++i) total_bits += bits[i];
    int shift = total_bits;
    for (int i = 0; i < count; ++i) {
        shift -= bits[i];
        uint64_t mask = (1ULL << bits[i]) - 1;
        out_fields[i] = static_cast<double>((val >> shift) & mask);
    }
    return 0;
}

void redis_free_value(shield_redis_value* value) {
    if (value && value->data) {
        std::free(const_cast<char*>(value->data));
        value->data = nullptr;
    }
}

void redis_free_zentries(shield_redis_zentry* entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; ++i) {
        if (entries[i].member) std::free(const_cast<char*>(entries[i].member));
    }
    std::free(entries);
}

// --- Plugin vtable -------------------------------------------------------

const shield_redis_plugin g_redis_plugin = {
    SHIELD_REDIS_ABI_VERSION,
    "redis",
    "1.0.0",

    redis_connect,
    redis_disconnect,
    redis_ping,

    redis_get,
    redis_set,
    redis_del,
    redis_exists,

    redis_incr,
    redis_decr,
    redis_incr_by,

    redis_hget,
    redis_hset,
    redis_hdel,
    redis_hgetall,

    redis_zadd,
    redis_zrem,
    redis_zscore,
    redis_zrank,
    redis_zrange,
    redis_zrevrange,
    redis_zrangebyscore,
    redis_zcount,

    redis_publish,
    redis_subscribe,
    redis_unsubscribe,

    redis_encode_composite,
    redis_decode_composite,

    redis_free_value,
    redis_free_zentries,
};

// --- Generic plugin wrapper ----------------------------------------------

const shield_plugin g_plugin = {
    SHIELD_PLUGIN_ABI_VERSION,
    SHIELD_PLUGIN_TYPE_REDIS,
    "shield_redis",
    "1.0.0",
    "Redis infrastructure plugin (key-value, hash, sorted set, pub/sub)",
    "Shield",

    // init
    [](const shield_host_t, const shield_host_api*,
       const shield_plugin_config* config,
       char* err_buf, int err_buf_size) -> int {
        shield_redis_config rc = {};
        for (int i = 0; i < config->count; ++i) {
            const char* key = config->items[i].key;
            const char* val = config->items[i].value;
            if (!key || !val) continue;
            if (std::strcmp(key, "host") == 0) rc.host = val;
            else if (std::strcmp(key, "port") == 0) rc.port = std::atoi(val);
            else if (std::strcmp(key, "password") == 0) rc.password = val;
            else if (std::strcmp(key, "db") == 0) rc.db = std::atoi(val);
            else if (std::strcmp(key, "pool_size") == 0) rc.pool_size = std::atoi(val);
            else if (std::strcmp(key, "connect_timeout_ms") == 0) rc.connect_timeout_ms = std::atoi(val);
            else if (std::strcmp(key, "command_timeout_ms") == 0) rc.command_timeout_ms = std::atoi(val);
        }
        auto* conn = redis_connect(&rc, err_buf, err_buf_size);
        return conn ? 0 : -1;
    },

    // shutdown
    []() { redis_disconnect(nullptr); },

    // capability_count
    []() -> int { return 1; },

    // get_capability
    [](int) -> const shield_plugin_capability* {
        static shield_plugin_capability cap = {"redis", "1.0.0", "Redis key-value/hash/sorted-set/pub-sub"};
        return &cap;
    },

    // vtable
    &g_redis_plugin,
};

}  // namespace

// Entry point.
extern "C" SHIELD_PLUGIN_EXPORT
const struct shield_plugin* shield_plugin_api(void) {
    return &g_plugin;
}
