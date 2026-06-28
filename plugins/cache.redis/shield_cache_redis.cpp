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
//
// Lua autonomy: register_lua installs the shared callable namespace
// shield.cache.redis(binding), returning a per-instance proxy whose
// methods reuse the same connect path as the C vtable.

#include "shield/plugin/abi.h"
#include "shield/plugin/cache.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/redis.h"

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

// shield_cache_conn is opaque in cache.h; concrete layout lives here.
// When redis_driver is set, all operations go through redis.driver's vtable.
// Otherwise, the conn uses its own sw::redis::Redis connection pool.
// NOTE: defined at global scope (not inside anonymous namespace) so that
// lambda-to-function-pointer conversion sees the same type as the vtable
// declared in cache.h.
struct shield_cache_conn {
    std::shared_ptr<sw::redis::Redis> redis;  // fallback: own connection
    const shield_redis_v1* redis_driver = nullptr;  // preferred: redis.driver
    void* redis_handle = nullptr;                    // opaque handle from driver
};

namespace {

char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

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

// ---------------------------------------------------------------------------
// v1 cache vtable
//
// open_redis() builds a shared sw::redis::Redis from the same config source
// (shield_cache_config for C callers, cache_instance for Lua callers). Both
// paths share the connection/pool logic.
// ---------------------------------------------------------------------------

std::shared_ptr<sw::redis::Redis> open_redis(
    const std::string& host, int port, const std::string& password,
    int db, int pool_size,
    int connect_timeout_ms, int command_timeout_ms,
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
        pool_opts.size = pool_size > 0 ? static_cast<std::size_t>(pool_size) : 8;

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
        [](const shield_cache_config* cfg,
           char* err_buf, int err_buf_size) -> shield_cache_conn* {
            // NOTE: This vtable is process-wide (static). The redis_driver
            // pointer is injected per-instance, so we cannot access it here.
            // The per-instance connect path (used by Lua and C++ callers)
            // goes through cache_instance::connect_instance() instead.
            // This legacy path is kept for direct C vtable callers who pass
            // their own config.
            if (!cfg) return nullptr;
            std::string err;
            std::string password = cfg->password ? cfg->password : "";
            std::string host = cfg->host && cfg->host[0] ? cfg->host : "localhost";
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
// v1 ABI entry. The instance carries its own config (parsed from config_json)
// and registers itself in a process-wide map so the Lua callable namespace
// `shield.cache.redis(binding)` can resolve it. The C++ vtable is still
// served through get_interface() for host code that goes through the cache
// pool / get_by_binding<shield_cache_v1>.
// ---------------------------------------------------------------------------
namespace {

struct cache_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* ctx = nullptr;
    // Parsed config (mirrors shield_cache_config keys).
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int db = 0;
    int pool_size = 8;
    int connect_timeout_ms = 5000;
    int command_timeout_ms = 5000;
    // Optional redis.driver dependency (Phase 2 migration).
    const shield_redis_v1* redis_driver = nullptr;
    void* redis_handle = nullptr;  // opaque handle from redis_driver->connect()
};

// Process-wide registry: instance_id -> cache_instance*. The callable Lua
// table's __call metamethod resolves binding -> instance_id, then looks up
// instances by id here. Map is read on every proxy creation, so it must be
// thread-safe.
std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, cache_instance*>& instances_map() {
    static std::map<std::string, cache_instance*> m;
    return m;
}

void register_instance(cache_instance* inst) {
    std::lock_guard lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}
void unregister_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    instances_map().erase(id);
}
cache_instance* find_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

// Parse the validated instance config_json. Tolerant — the host already
// checked against config_schema, so we only extract known keys and fall back
// to defaults for anything missing.
void parse_instance_config(cache_instance* inst, const char* config_json) {
    if (!config_json || !config_json[0]) return;
    try {
        auto j = nlohmann::json::parse(config_json);
        if (j.contains("host") && j["host"].is_string()) {
            inst->host = j["host"].get<std::string>();
        }
        if (j.contains("port") && j["port"].is_number_integer()) {
            inst->port = j["port"].get<int>();
        }
        if (j.contains("password") && j["password"].is_string()) {
            inst->password = j["password"].get<std::string>();
        }
        if (j.contains("db") && j["db"].is_number_integer()) {
            inst->db = j["db"].get<int>();
        }
        if (j.contains("pool_size") && j["pool_size"].is_number_integer()) {
            inst->pool_size = j["pool_size"].get<int>();
        }
        if (j.contains("connect_timeout_ms") &&
            j["connect_timeout_ms"].is_number_integer()) {
            inst->connect_timeout_ms = j["connect_timeout_ms"].get<int>();
        }
        if (j.contains("command_timeout_ms") &&
            j["command_timeout_ms"].is_number_integer()) {
            inst->command_timeout_ms = j["command_timeout_ms"].get<int>();
        }
    } catch (...) {
        // Malformed JSON shouldn't happen (host validated), ignore quietly.
    }
}

// Build a shared sw::redis::Redis from an instance config. Returns nullptr on
// failure (and fills err).
std::shared_ptr<sw::redis::Redis> open_redis(const cache_instance* inst,
                                             std::string* err) {
    return open_redis(inst->host, inst->port, inst->password, inst->db,
                      inst->pool_size, inst->connect_timeout_ms,
                      inst->command_timeout_ms, err);
}

// ---------------------------------------------------------------------------
// Lua helpers.
//
// Each proxy call opens a sw::redis::Redis (with internal pool), runs the
// command, then drops the shared_ptr — the underlying pool keeps the TCP
// connection warm across calls. open_redis() returns nullptr on failure.
// ---------------------------------------------------------------------------

// Build a Lua error table {code=..., message=...} matching the shape used by
// the host's shield.cache.* facade.
sol::table make_error_table(sol::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}

// Extract a Lua string from a shield_redis_value_v1 (STRING or NIL -> nil).
// Returns sol::nil for NIL/NULL, or a Lua string for STRING type.
static sol::object redis_value_to_lua_string(sol::state_view lua,
                                              const shield_redis_value_v1* v) {
    if (!v || v->type == SHIELD_REDIS_NIL) return sol::nil;
    if (v->type == SHIELD_REDIS_STRING && v->str) {
        return sol::make_object(lua,
            std::string(v->str, static_cast<size_t>(v->str_len)));
    }
    return sol::nil;
}

// Build a per-instance proxy table. Each method opens a redis handle, runs
// the command, then drops the shared_ptr. Return shape:
//   success: (true, ...payload...)
//   failure: (false, {code=..., message=...})
sol::table make_instance_proxy(sol::state_view lua, cache_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function("get",
        [inst](sol::this_state s, std::string key) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            // redis.driver path
            if (inst->redis_driver && inst->redis_handle) {
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                int rc = inst->redis_driver->get(
                    inst->redis_handle, key.c_str(), &out, &err);
                if (rc == 0) {
                    results.push_back(sol::make_object(lua, true));
                    results.push_back(redis_value_to_lua_string(lua, out));
                } else {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(lua,
                        err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "get failed"));
                }
                if (out) inst->redis_driver->free_value(out);
                return results;
            }
            // Fallback: internal redis++ connection
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "connection_failed", err));
                return results;
            }
            try {
                auto v = redis->get(key);
                results.push_back(sol::make_object(lua, true));
                if (v) {
                    results.push_back(sol::make_object(lua, *v));
                } else {
                    results.push_back(sol::nil);
                }
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function("set",
        [inst](sol::this_state s, std::string key, std::string value,
               sol::optional<lua_Integer> ttl) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                shield_error_v1 err{};
                int ttl_sec = ttl ? static_cast<int>(*ttl) : 0;
                int rc = inst->redis_driver->set(
                    inst->redis_handle, key.c_str(), value.c_str(),
                    ttl_sec, &err);
                if (rc == 0) {
                    results.push_back(sol::make_object(lua, true));
                } else {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(lua,
                        err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "set failed"));
                }
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                if (ttl && *ttl > 0) redis->set(key, value, std::chrono::seconds(*ttl));
                else redis->set(key, value);
                results.push_back(sol::make_object(lua, true));
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function("del",
        [inst](sol::this_state s, std::string key) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                shield_error_v1 err{};
                int rc = inst->redis_driver->del(
                    inst->redis_handle, key.c_str(), &err);
                if (rc == 0) {
                    results.push_back(sol::make_object(lua, true));
                } else {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(lua,
                        err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "del failed"));
                }
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto removed = redis->del(key);
                results.push_back(sol::make_object(lua, true));
                results.push_back(sol::make_object(lua, static_cast<lua_Integer>(removed)));
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    // exists, incr, incr_by, hdel: use redis.driver raw command for operations
    // not in the typed shield.redis.v1 API.

    proxy.set_function("exists",
        [inst](sol::this_state s, std::string key) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                std::string k = key;
                shield_redis_arg_v1 args[] = {
                    {"EXISTS", 6}, {k.data(), k.size()}};
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                int rc = inst->redis_driver->command(
                    inst->redis_handle, args, 2, &out, &err);
                if (rc == 0 && out) {
                    results.push_back(sol::make_object(lua, true));
                    bool exists = (out->type == SHIELD_REDIS_INTEGER && out->integer > 0);
                    results.push_back(sol::make_object(lua, exists));
                } else {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(lua,
                        err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "exists failed"));
                }
                if (out) inst->redis_driver->free_value(out);
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto n = redis->exists(key);
                results.push_back(sol::make_object(lua, true));
                results.push_back(sol::make_object(lua, n > 0));
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function("incr",
        [inst](sol::this_state s, std::string key) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                std::string k = key;
                shield_redis_arg_v1 args[] = {
                    {"INCR", 4}, {k.data(), k.size()}};
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                int rc = inst->redis_driver->command(
                    inst->redis_handle, args, 2, &out, &err);
                if (rc == 0 && out) {
                    results.push_back(sol::make_object(lua, true));
                    results.push_back(sol::make_object(lua,
                        static_cast<lua_Integer>(out->integer)));
                } else {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(lua,
                        err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "incr failed"));
                }
                if (out) inst->redis_driver->free_value(out);
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto v = redis->incr(key);
                results.push_back(sol::make_object(lua, true));
                results.push_back(sol::make_object(lua, static_cast<lua_Integer>(v)));
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function("incr_by",
        [inst](sol::this_state s, std::string key,
               lua_Integer amount) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                std::string k = key;
                std::string amt = std::to_string(static_cast<long long>(amount));
                shield_redis_arg_v1 args[] = {
                    {"INCRBY", 6}, {k.data(), k.size()},
                    {amt.data(), amt.size()}};
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                int rc = inst->redis_driver->command(
                    inst->redis_handle, args, 3, &out, &err);
                if (rc == 0 && out) {
                    results.push_back(sol::make_object(lua, true));
                    results.push_back(sol::make_object(lua,
                        static_cast<lua_Integer>(out->integer)));
                } else {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(lua,
                        err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "incr_by failed"));
                }
                if (out) inst->redis_driver->free_value(out);
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto v = redis->incrby(key, static_cast<long long>(amount));
                results.push_back(sol::make_object(lua, true));
                results.push_back(sol::make_object(lua, static_cast<lua_Integer>(v)));
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function("hget",
        [inst](sol::this_state s, std::string key,
               std::string field) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                int rc = inst->redis_driver->hget(
                    inst->redis_handle, key.c_str(), field.c_str(), &out, &err);
                if (rc == 0) {
                    results.push_back(sol::make_object(lua, true));
                    results.push_back(redis_value_to_lua_string(lua, out));
                } else {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(lua,
                        err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "hget failed"));
                }
                if (out) inst->redis_driver->free_value(out);
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto v = redis->hget(key, field);
                results.push_back(sol::make_object(lua, true));
                if (v) results.push_back(sol::make_object(lua, *v));
                else results.push_back(sol::nil);
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function("hset",
        [inst](sol::this_state s, std::string key, std::string field,
               std::string value) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                shield_error_v1 err{};
                int rc = inst->redis_driver->hset(
                    inst->redis_handle, key.c_str(), field.c_str(),
                    value.c_str(), &err);
                if (rc == 0) {
                    results.push_back(sol::make_object(lua, true));
                } else {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(lua,
                        err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "hset failed"));
                }
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                redis->hset(key, field, value);
                results.push_back(sol::make_object(lua, true));
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function("hdel",
        [inst](sol::this_state s, std::string key,
               std::string field) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                std::string k = key, f = field;
                shield_redis_arg_v1 args[] = {
                    {"HDEL", 4}, {k.data(), k.size()}, {f.data(), f.size()}};
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                int rc = inst->redis_driver->command(
                    inst->redis_handle, args, 3, &out, &err);
                if (rc == 0) {
                    results.push_back(sol::make_object(lua, true));
                    if (out && out->type == SHIELD_REDIS_INTEGER)
                        results.push_back(sol::make_object(lua,
                            static_cast<lua_Integer>(out->integer)));
                } else {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error_table(lua,
                        err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "hdel failed"));
                }
                if (out) inst->redis_driver->free_value(out);
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto removed = redis->hdel(key, field);
                results.push_back(sol::make_object(lua, true));
                results.push_back(sol::make_object(lua, static_cast<lua_Integer>(removed)));
            } catch (const std::exception& e) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    return proxy;
}

int register_lua_impl(shield_plugin_instance_v1* self,
                      struct lua_State* L,
                      shield_error_v1* err) {
    // register_lua installs the shared, idempotent callable namespace
    // shield.cache.redis. Lua passes a binding logical name; host config
    // resolves that binding to the deployment instance id.
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "cache.redis: lua_State is null";
        }
        return 1;
    }
    auto* current = reinterpret_cast<cache_instance*>(self);
    if (!current || !current->host_api ||
        !current->host_api->binding_instance_id) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "cache.redis: host binding resolver is null";
        }
        return 1;
    }
    sol::state_view lua(L);

    // Build the callable namespace shield.cache.redis.
    auto shield = lua["shield"].get_or_create<sol::table>();
    auto cache = shield["cache"].get_or_create<sol::table>();

    sol::object existing = cache["redis"];
    if (!existing.is<sol::table>()) {
        auto ns = lua.create_table();
        auto mt = lua.create_table();
        const shield_host_api_v1* host_api = current->host_api;
        shield_plugin_context_v1* ctx = current->ctx;
        mt.set_function("__call",
            [host_api, ctx](sol::this_state s, sol::table /*self*/,
               std::string binding) -> sol::object {
                sol::state_view lua(s);
                const char* instance_id =
                    host_api->binding_instance_id(ctx, binding.c_str());
                if (!instance_id) return sol::nil;
                auto* inst = find_instance(instance_id);
                if (!inst) return sol::nil;
                return sol::make_object(lua, make_instance_proxy(lua, inst));
            });
        ns[sol::metatable_key] = mt;
        cache["redis"] = ns;
    }

    return 0;
}

int cache_create(const shield_plugin_create_args_v1* args,
                 shield_plugin_instance_v1** out,
                 shield_error_v1* err) {
    (void)err;
    auto* inst = new cache_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->host_api = args->host_api;
    inst->ctx = args->ctx;
    parse_instance_config(inst, args->config_json);
    register_instance(inst);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1*,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_CACHE_INTERFACE) == 0)
            return &cache_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1* self,
                           shield_error_v1*) -> int {
        auto* ci = reinterpret_cast<cache_instance*>(self);
        // Try to get redis.driver dependency (optional — Phase 2 migration).
        if (ci->host_api && ci->host_api->dependency) {
            auto* drv = static_cast<const shield_redis_v1*>(
                ci->host_api->dependency(ci->ctx, "redis", SHIELD_REDIS_V1));
            if (drv && drv->connect) {
                char err_buf[256] = {};
                void* handle = drv->connect(nullptr, err_buf, sizeof(err_buf));
                if (handle) {
                    ci->redis_driver = drv;
                    ci->redis_handle = handle;
                }
                // If connect fails, fall through to internal redis path.
            }
        }
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        auto* ci = reinterpret_cast<cache_instance*>(self);
        if (ci->redis_driver && ci->redis_handle) {
            ci->redis_driver->disconnect(ci->redis_handle);
            ci->redis_handle = nullptr;
            ci->redis_driver = nullptr;
        }
        unregister_instance(ci->instance_id);
        delete ci;
    };
    inst->shell.register_lua = &register_lua_impl;
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
