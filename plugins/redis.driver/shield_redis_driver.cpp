// [SHIELD_PLUGIN] redis.driver — Unified Redis driver for shield.redis.v1.
//
// v1 ABI. Holds a single sw::redis::Redis connection pool per instance and
// exposes typed Redis commands through the shield.redis_v1 C ABI vtable.
// Other Redis plugins (cache.redis, queue.redis, leaderboard.redis) depend
// on this plugin instead of creating their own Redis connections.
//
// The typed API covers the most common Redis operations: key-value (get/set/
// del), hash (hget/hset/hgetall), sorted set (zadd/zrange). The pipeline()
// method batches multiple commands in a single round-trip. The command()
// method is the raw escape hatch for any Redis command not covered by typed
// methods (EVAL, XADD, INFO, etc.).
//
// Lua autonomy: register_lua() installs the callable namespace
// shield.redis(binding), returning a per-instance proxy whose methods
// reuse the same sw::redis::Redis connection pool as the C vtable.

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/redis.h"

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

char* dup_string(const char* s) {
    if (!s) return nullptr;
    auto len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (out) std::memcpy(out, s, len + 1);
    return out;
}

char* dup_bytes(const void* data, uint64_t len) {
    if (!data || len == 0) return nullptr;
    char* out = static_cast<char*>(std::malloc(static_cast<size_t>(len)));
    if (out) std::memcpy(out, data, static_cast<size_t>(len));
    return out;
}

// Allocate a single shield_redis_value_v1 on the heap.
shield_redis_value_v1* alloc_value() {
    auto* v = static_cast<shield_redis_value_v1*>(
        std::calloc(1, sizeof(shield_redis_value_v1)));
    return v;
}

// Build a STRING value from a std::string.
shield_redis_value_v1* make_string_value(const std::string& s) {
    auto* v = alloc_value();
    if (!v) return nullptr;
    v->type = SHIELD_REDIS_STRING;
    v->str = dup_bytes(s.data(), s.size());
    v->str_len = s.size();
    return v;
}

// Build an INTEGER value.
shield_redis_value_v1* make_integer_value(int64_t n) {
    auto* v = alloc_value();
    if (!v) return nullptr;
    v->type = SHIELD_REDIS_INTEGER;
    v->integer = n;
    return v;
}

// Build a NIL value.
shield_redis_value_v1* make_nil_value() {
    auto* v = alloc_value();
    if (!v) return nullptr;
    v->type = SHIELD_REDIS_NIL;
    return v;
}

// Build an ERROR value from a string.
shield_redis_value_v1* make_error_value(const std::string& msg) {
    auto* v = alloc_value();
    if (!v) return nullptr;
    v->type = SHIELD_REDIS_ERROR;
    v->str = dup_string(msg.c_str());
    v->str_len = msg.size();
    return v;
}

// Recursively convert a hiredis redisReply to a shield_redis_value_v1.
// redis++ returns replies as std::unique_ptr<redisReply> (ReplyUPtr).
// We work with the raw redisReply* from hiredis directly.
shield_redis_value_v1* reply_to_value(const redisReply* reply) {
    if (!reply) return make_nil_value();
    switch (reply->type) {
        case REDIS_REPLY_NIL:
            return make_nil_value();
        case REDIS_REPLY_STRING:
        case REDIS_REPLY_STATUS:
            return make_string_value(
                std::string(reply->str, static_cast<size_t>(reply->len)));
        case REDIS_REPLY_INTEGER:
            return make_integer_value(reply->integer);
        case REDIS_REPLY_ERROR:
            return make_error_value(
                std::string(reply->str, static_cast<size_t>(reply->len)));
        case REDIS_REPLY_ARRAY: {
            auto* v = alloc_value();
            if (!v) return nullptr;
            v->type = SHIELD_REDIS_ARRAY;
            v->item_count = reply->elements;
            if (reply->elements == 0 || reply->element == nullptr) {
                v->items = nullptr;
            } else {
                v->items = static_cast<shield_redis_value_v1*>(
                    std::calloc(reply->elements,
                                sizeof(shield_redis_value_v1)));
                for (size_t i = 0; i < reply->elements; ++i) {
                    auto* item = reply_to_value(reply->element[i]);
                    if (item) {
                        v->items[i] = *item;
                        std::free(item);  // shallow copy into array
                    }
                }
            }
            return v;
        }
        default:
            return make_nil_value();
    }
}

// ---------------------------------------------------------------------------
// Instance config
// ---------------------------------------------------------------------------

struct redis_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    const shield_host_api_v1* host_api = nullptr;
    shield_plugin_context_v1* ctx = nullptr;

    // Parsed config
    std::string mode = "single";
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string password;
    int db = 0;
    int pool_size = 8;
    int connect_timeout_ms = 5000;
    int command_timeout_ms = 3000;

    // The redis++ client, created in start().
    std::shared_ptr<sw::redis::Redis> redis;
};

void parse_instance_config(redis_instance* inst, const char* config_json) {
    if (!config_json || !config_json[0]) return;
    try {
        auto j = nlohmann::json::parse(config_json);
        if (j.contains("mode") && j["mode"].is_string())
            inst->mode = j["mode"].get<std::string>();
        if (j.contains("host") && j["host"].is_string())
            inst->host = j["host"].get<std::string>();
        if (j.contains("port") && j["port"].is_number_integer())
            inst->port = j["port"].get<int>();
        if (j.contains("password") && j["password"].is_string())
            inst->password = j["password"].get<std::string>();
        if (j.contains("db") && j["db"].is_number_integer())
            inst->db = j["db"].get<int>();
        if (j.contains("pool_size") && j["pool_size"].is_number_integer())
            inst->pool_size = j["pool_size"].get<int>();
        if (j.contains("connect_timeout_ms") &&
            j["connect_timeout_ms"].is_number_integer())
            inst->connect_timeout_ms = j["connect_timeout_ms"].get<int>();
        if (j.contains("command_timeout_ms") &&
            j["command_timeout_ms"].is_number_integer())
            inst->command_timeout_ms = j["command_timeout_ms"].get<int>();
    } catch (...) {
        // Host already validated config; ignore parse errors.
    }
}

// Build a sw::redis::Redis from the instance config.
bool open_redis(redis_instance* inst, std::string* err) {
    try {
        sw::redis::ConnectionOptions opts;
        opts.host = inst->host.empty() ? "localhost" : inst->host;
        opts.port = inst->port > 0 ? inst->port : 6379;
        if (!inst->password.empty()) opts.password = inst->password;
        opts.db = inst->db > 0 ? inst->db : 0;
        opts.connect_timeout = std::chrono::milliseconds(
            inst->connect_timeout_ms > 0 ? inst->connect_timeout_ms : 5000);
        opts.socket_timeout = std::chrono::milliseconds(
            inst->command_timeout_ms > 0 ? inst->command_timeout_ms : 3000);

        sw::redis::ConnectionPoolOptions pool_opts;
        pool_opts.size =
            inst->pool_size > 0 ? static_cast<std::size_t>(inst->pool_size) : 8;

        inst->redis = std::make_shared<sw::redis::Redis>(opts, pool_opts);
        inst->redis->ping();
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// Process-wide instance registry (for Lua proxy resolution)
// ---------------------------------------------------------------------------

std::mutex& instances_mu() {
    static std::mutex m;
    return m;
}
std::map<std::string, redis_instance*>& instances_map() {
    static std::map<std::string, redis_instance*> m;
    return m;
}

void register_instance(redis_instance* inst) {
    std::lock_guard lk(instances_mu());
    instances_map()[inst->instance_id] = inst;
}
void unregister_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    instances_map().erase(id);
}
redis_instance* find_instance(const std::string& id) {
    std::lock_guard lk(instances_mu());
    auto it = instances_map().find(id);
    return it == instances_map().end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// v1 vtable — typed methods
// ---------------------------------------------------------------------------

const shield_redis_v1& redis_vtable() {
    static const shield_redis_v1 v = {
        sizeof(shield_redis_v1),
        SHIELD_REDIS_V1,

        // connect — returns the driver instance as an opaque handle.
        // cfg is ignored (the driver uses its own config from startup).
        // Dependent plugins call this once to get a handle for all methods.
        [](const void* /*cfg*/, char* err_buf,
           int err_buf_size) -> void* {
            // Find the first (and typically only) started redis.driver instance.
            std::lock_guard lk(instances_mu());
            for (auto& [id, inst] : instances_map()) {
                if (inst && inst->redis) {
                    return static_cast<void*>(inst);
                }
            }
            if (err_buf && err_buf_size > 0) {
                std::snprintf(err_buf, err_buf_size,
                              "redis.driver: no started instance available");
            }
            return nullptr;
        },

        // disconnect — no-op. The pool is owned by the driver instance.
        [](void* /*handle*/) {},

        // get
        [](void* inst, const char* key,
           shield_redis_value_v1** out, shield_error_v1* err) -> int {
            if (!inst || !key || !out) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                auto val = ri->redis->get(std::string(key));
                if (val) {
                    *out = make_string_value(*val);
                } else {
                    *out = make_nil_value();
                }
                return 0;
            } catch (const std::exception& e) {
                if (err) { err->code = "redis.command.failed"; err->message = e.what(); }
                *out = make_nil_value();
                return -1;
            }
        },

        // set
        [](void* inst, const char* key, const char* val,
           int ttl_sec, shield_error_v1* err) -> int {
            if (!inst || !key || !val) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                if (ttl_sec > 0) {
                    ri->redis->set(std::string(key), std::string(val),
                                   std::chrono::seconds(ttl_sec));
                } else {
                    ri->redis->set(std::string(key), std::string(val));
                }
                return 0;
            } catch (const std::exception& e) {
                if (err) { err->code = "redis.command.failed"; err->message = e.what(); }
                return -1;
            }
        },

        // del
        [](void* inst, const char* key, shield_error_v1* err) -> int {
            if (!inst || !key) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                ri->redis->del(std::string(key));
                return 0;
            } catch (const std::exception& e) {
                if (err) { err->code = "redis.command.failed"; err->message = e.what(); }
                return -1;
            }
        },

        // hget
        [](void* inst, const char* key, const char* field,
           shield_redis_value_v1** out, shield_error_v1* err) -> int {
            if (!inst || !key || !field || !out) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                auto val = ri->redis->hget(std::string(key), std::string(field));
                if (val) {
                    *out = make_string_value(*val);
                } else {
                    *out = make_nil_value();
                }
                return 0;
            } catch (const std::exception& e) {
                if (err) { err->code = "redis.command.failed"; err->message = e.what(); }
                *out = make_nil_value();
                return -1;
            }
        },

        // hset
        [](void* inst, const char* key, const char* field,
           const char* val, shield_error_v1* err) -> int {
            if (!inst || !key || !field || !val) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                ri->redis->hset(std::string(key), std::string(field),
                                std::string(val));
                return 0;
            } catch (const std::exception& e) {
                if (err) { err->code = "redis.command.failed"; err->message = e.what(); }
                return -1;
            }
        },

        // hgetall
        [](void* inst, const char* key,
           shield_redis_value_v1** out, shield_error_v1* err) -> int {
            if (!inst || !key || !out) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                std::map<std::string, std::string> result;
                ri->redis->hgetall(std::string(key),
                                   std::inserter(result, result.begin()));
                auto* v = alloc_value();
                if (!v) return -1;
                v->type = SHIELD_REDIS_ARRAY;
                v->item_count = result.size() * 2;
                if (result.empty()) {
                    v->items = nullptr;
                } else {
                    v->items = static_cast<shield_redis_value_v1*>(
                        std::calloc(v->item_count, sizeof(shield_redis_value_v1)));
                    size_t i = 0;
                    for (const auto& [f, val] : result) {
                        // field
                        v->items[i].type = SHIELD_REDIS_STRING;
                        v->items[i].str = dup_bytes(f.data(), f.size());
                        v->items[i].str_len = f.size();
                        ++i;
                        // value
                        v->items[i].type = SHIELD_REDIS_STRING;
                        v->items[i].str = dup_bytes(val.data(), val.size());
                        v->items[i].str_len = val.size();
                        ++i;
                    }
                }
                *out = v;
                return 0;
            } catch (const std::exception& e) {
                if (err) { err->code = "redis.command.failed"; err->message = e.what(); }
                *out = make_nil_value();
                return -1;
            }
        },

        // zadd
        [](void* inst, const char* key, double score,
           const char* member, shield_error_v1* err) -> int {
            if (!inst || !key || !member) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                ri->redis->zadd(std::string(key), std::string(member), score);
                return 0;
            } catch (const std::exception& e) {
                if (err) { err->code = "redis.command.failed"; err->message = e.what(); }
                return -1;
            }
        },

        // zrange
        [](void* inst, const char* key, int start, int stop,
           shield_redis_value_v1** out, shield_error_v1* err) -> int {
            if (!inst || !key || !out) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                std::vector<std::string> result;
                ri->redis->zrange(std::string(key), start, stop,
                                  std::back_inserter(result));
                auto* v = alloc_value();
                if (!v) return -1;
                v->type = SHIELD_REDIS_ARRAY;
                v->item_count = result.size();
                if (result.empty()) {
                    v->items = nullptr;
                } else {
                    v->items = static_cast<shield_redis_value_v1*>(
                        std::calloc(result.size(), sizeof(shield_redis_value_v1)));
                    for (size_t i = 0; i < result.size(); ++i) {
                        v->items[i].type = SHIELD_REDIS_STRING;
                        v->items[i].str = dup_bytes(result[i].data(),
                                                    result[i].size());
                        v->items[i].str_len = result[i].size();
                    }
                }
                *out = v;
                return 0;
            } catch (const std::exception& e) {
                if (err) { err->code = "redis.command.failed"; err->message = e.what(); }
                *out = make_nil_value();
                return -1;
            }
        },

        // pipeline
        [](void* inst, const shield_redis_command_v1* cmds, uint64_t count,
           shield_redis_value_v1** out_array, uint64_t* out_count,
           shield_error_v1* err) -> int {
            if (!inst || !cmds || !out_array || !out_count) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                auto pipe = ri->redis->pipeline();
                for (uint64_t i = 0; i < count; ++i) {
                    const auto& cmd = cmds[i];
                    if (cmd.argc == 0 || !cmd.args) continue;
                    std::vector<std::string> argv;
                    for (uint64_t a = 0; a < cmd.argc; ++a) {
                        argv.emplace_back(
                            static_cast<const char*>(cmd.args[a].data),
                            static_cast<size_t>(cmd.args[a].len));
                    }
                    pipe.command(argv.begin(), argv.end());
                }
                // exec() returns QueuedReplies; use .get(idx) for each reply.
                auto replies = pipe.exec();
                *out_count = replies.size();
                *out_array = static_cast<shield_redis_value_v1*>(
                    std::calloc(replies.size(), sizeof(shield_redis_value_v1)));
                for (size_t i = 0; i < replies.size(); ++i) {
                    auto* val = reply_to_value(&replies.get(i));
                    if (val) {
                        (*out_array)[i] = *val;
                        std::free(val);  // shallow copy into array
                    }
                }
                return 0;
            } catch (const std::exception& e) {
                if (err) { err->code = "redis.pipeline.failed"; err->message = e.what(); }
                return -1;
            }
        },

        // command (raw escape hatch)
        [](void* inst, const shield_redis_arg_v1* args, uint64_t argc,
           shield_redis_value_v1** out, shield_error_v1* err) -> int {
            if (!inst || !args || argc == 0 || !out) return -1;
            auto* ri = static_cast<redis_instance*>(inst);
            if (!ri->redis) return -1;
            try {
                std::vector<std::string> argv;
                for (uint64_t i = 0; i < argc; ++i) {
                    argv.emplace_back(
                        static_cast<const char*>(args[i].data),
                        static_cast<size_t>(args[i].len));
                }
                // command() returns ReplyUPtr (unique_ptr<redisReply>)
                auto reply = ri->redis->command(argv.begin(), argv.end());
                *out = reply_to_value(reply.get());
                return 0;
            } catch (const std::exception& e) {
                if (err) { err->code = "redis.command.failed"; err->message = e.what(); }
                *out = make_nil_value();
                return -1;
            }
        },

        // free_value
        [](shield_redis_value_v1* value) {
            if (!value) return;
            if (value->str) {
                std::free(const_cast<char*>(value->str));
                value->str = nullptr;
            }
            if (value->items) {
                for (uint64_t i = 0; i < value->item_count; ++i) {
                    // Recursive free for nested arrays.
                    if (value->items[i].str) {
                        std::free(const_cast<char*>(value->items[i].str));
                    }
                    if (value->items[i].items) {
                        // One level of nesting is enough for typical Redis
                        // responses; deeper nesting is freed recursively.
                        for (uint64_t j = 0; j < value->items[i].item_count; ++j) {
                            if (value->items[i].items[j].str) {
                                std::free(const_cast<char*>(
                                    value->items[i].items[j].str));
                            }
                            if (value->items[i].items[j].items) {
                                std::free(value->items[i].items[j].items);
                            }
                        }
                        std::free(value->items[i].items);
                    }
                }
                std::free(value->items);
                value->items = nullptr;
            }
            value->str_len = 0;
            value->item_count = 0;
        },
    };
    return v;
}

// ---------------------------------------------------------------------------
// Lua helpers
// ---------------------------------------------------------------------------

sol::table make_error_table(sol::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}

// Convert a shield_redis_value_v1 to a Lua object.
sol::object value_to_lua(sol::state_view lua,
                         const shield_redis_value_v1* v) {
    if (!v) return sol::nil;
    switch (v->type) {
        case SHIELD_REDIS_NIL:
            return sol::nil;
        case SHIELD_REDIS_STRING:
            return sol::make_object(lua,
                std::string(v->str, static_cast<size_t>(v->str_len)));
        case SHIELD_REDIS_INTEGER:
            return sol::make_object(lua, static_cast<lua_Integer>(v->integer));
        case SHIELD_REDIS_DOUBLE:
            return sol::make_object(lua, v->number);
        case SHIELD_REDIS_BOOL:
            return sol::make_object(lua, v->boolean != 0);
        case SHIELD_REDIS_ARRAY: {
            auto tbl = lua.create_table();
            for (uint64_t i = 0; i < v->item_count; ++i) {
                tbl[i + 1] = value_to_lua(lua, &v->items[i]);
            }
            return sol::make_object(lua, tbl);
        }
        case SHIELD_REDIS_ERROR:
            return sol::make_object(lua,
                std::string(v->str, static_cast<size_t>(v->str_len)));
    }
    return sol::nil;
}

// Convert a Lua object to a string for command arguments.
std::string lua_to_string(sol::object obj) {
    if (obj.is<std::string>()) return obj.as<std::string>();
    if (obj.is<lua_Integer>()) return std::to_string(obj.as<lua_Integer>());
    if (obj.is<double>()) return std::to_string(obj.as<double>());
    if (obj.is<bool>()) return obj.as<bool>() ? "1" : "0";
    return "";
}

// Build a per-instance Lua proxy.
sol::table make_instance_proxy(sol::state_view lua, redis_instance* inst) {
    auto proxy = lua.create_table();
    const shield_redis_v1& v = redis_vtable();

    // get(key) -> ok, value|nil|error
    proxy.set_function("get",
        [&v, inst](sol::this_state s, std::string key) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            shield_redis_value_v1* out = nullptr;
            shield_error_v1 err{};
            int rc = v.get(inst, key.c_str(), &out, &err);
            if (rc == 0) {
                results.push_back(sol::make_object(lua, true));
                if (out && out->type != SHIELD_REDIS_NIL) {
                    results.push_back(value_to_lua(lua, out));
                } else {
                    results.push_back(sol::nil);
                }
            } else {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua,
                    err.code ? err.code : "redis.error",
                    err.message ? err.message : "get failed"));
            }
            if (out) v.free_value(out);
            return results;
        });

    // set(key, value [, ttl]) -> ok, error
    proxy.set_function("set",
        [&v, inst](sol::this_state s, std::string key, std::string value,
                   sol::optional<lua_Integer> ttl) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            shield_error_v1 err{};
            int ttl_sec = ttl ? static_cast<int>(*ttl) : 0;
            int rc = v.set(inst, key.c_str(), value.c_str(), ttl_sec, &err);
            if (rc == 0) {
                results.push_back(sol::make_object(lua, true));
            } else {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua,
                    err.code ? err.code : "redis.error",
                    err.message ? err.message : "set failed"));
            }
            return results;
        });

    // del(key) -> ok, error
    proxy.set_function("del",
        [&v, inst](sol::this_state s, std::string key) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            shield_error_v1 err{};
            int rc = v.del(inst, key.c_str(), &err);
            if (rc == 0) {
                results.push_back(sol::make_object(lua, true));
            } else {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua,
                    err.code ? err.code : "redis.error",
                    err.message ? err.message : "del failed"));
            }
            return results;
        });

    // hget(key, field) -> ok, value|nil|error
    proxy.set_function("hget",
        [&v, inst](sol::this_state s, std::string key,
                   std::string field) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            shield_redis_value_v1* out = nullptr;
            shield_error_v1 err{};
            int rc = v.hget(inst, key.c_str(), field.c_str(), &out, &err);
            if (rc == 0) {
                results.push_back(sol::make_object(lua, true));
                if (out && out->type != SHIELD_REDIS_NIL) {
                    results.push_back(value_to_lua(lua, out));
                } else {
                    results.push_back(sol::nil);
                }
            } else {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua,
                    err.code ? err.code : "redis.error",
                    err.message ? err.message : "hget failed"));
            }
            if (out) v.free_value(out);
            return results;
        });

    // hset(key, field, value) -> ok, error
    proxy.set_function("hset",
        [&v, inst](sol::this_state s, std::string key, std::string field,
                   std::string value) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            shield_error_v1 err{};
            int rc = v.hset(inst, key.c_str(), field.c_str(),
                            value.c_str(), &err);
            if (rc == 0) {
                results.push_back(sol::make_object(lua, true));
            } else {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua,
                    err.code ? err.code : "redis.error",
                    err.message ? err.message : "hset failed"));
            }
            return results;
        });

    // hgetall(key) -> ok, {field=value,...}|error
    proxy.set_function("hgetall",
        [&v, inst](sol::this_state s, std::string key) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            shield_redis_value_v1* out = nullptr;
            shield_error_v1 err{};
            int rc = v.hgetall(inst, key.c_str(), &out, &err);
            if (rc == 0 && out && out->type == SHIELD_REDIS_ARRAY) {
                results.push_back(sol::make_object(lua, true));
                auto tbl = lua.create_table();
                // hgetall returns alternating [field, value, field, value, ...]
                for (uint64_t i = 0; i + 1 < out->item_count; i += 2) {
                    auto& f = out->items[i];
                    auto& val = out->items[i + 1];
                    if (f.str && val.str) {
                        tbl[std::string(f.str, static_cast<size_t>(f.str_len))] =
                            std::string(val.str, static_cast<size_t>(val.str_len));
                    }
                }
                results.push_back(sol::make_object(lua, tbl));
            } else if (rc == 0) {
                results.push_back(sol::make_object(lua, true));
                results.push_back(sol::make_object(lua, lua.create_table()));
            } else {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua,
                    err.code ? err.code : "redis.error",
                    err.message ? err.message : "hgetall failed"));
            }
            if (out) v.free_value(out);
            return results;
        });

    // zadd(key, score, member) -> ok, error
    proxy.set_function("zadd",
        [&v, inst](sol::this_state s, std::string key, double score,
                   std::string member) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            shield_error_v1 err{};
            int rc = v.zadd(inst, key.c_str(), score, member.c_str(), &err);
            if (rc == 0) {
                results.push_back(sol::make_object(lua, true));
            } else {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua,
                    err.code ? err.code : "redis.error",
                    err.message ? err.message : "zadd failed"));
            }
            return results;
        });

    // zrange(key, start, stop) -> ok, {member,...}|error
    proxy.set_function("zrange",
        [&v, inst](sol::this_state s, std::string key, int start,
                   int stop) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;
            shield_redis_value_v1* out = nullptr;
            shield_error_v1 err{};
            int rc = v.zrange(inst, key.c_str(), start, stop, &out, &err);
            if (rc == 0 && out && out->type == SHIELD_REDIS_ARRAY) {
                results.push_back(sol::make_object(lua, true));
                auto tbl = lua.create_table();
                for (uint64_t i = 0; i < out->item_count; ++i) {
                    auto& item = out->items[i];
                    if (item.str) {
                        tbl[i + 1] = std::string(
                            item.str, static_cast<size_t>(item.str_len));
                    }
                }
                results.push_back(sol::make_object(lua, tbl));
            } else if (rc == 0) {
                results.push_back(sol::make_object(lua, true));
                results.push_back(sol::make_object(lua, lua.create_table()));
            } else {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua,
                    err.code ? err.code : "redis.error",
                    err.message ? err.message : "zrange failed"));
            }
            if (out) v.free_value(out);
            return results;
        });

    // command(cmd, ...) -> ok, result|error
    proxy.set_function("command",
        [&v, inst](sol::this_state s, sol::variadic_args va) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;

            std::vector<std::string> args_storage;
            for (auto arg : va) {
                args_storage.push_back(lua_to_string(arg));
            }
            if (args_storage.empty()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua,
                    "invalid_args", "command requires at least 1 argument"));
                return results;
            }

            std::vector<shield_redis_arg_v1> c_args(args_storage.size());
            for (size_t i = 0; i < args_storage.size(); ++i) {
                c_args[i].data = args_storage[i].data();
                c_args[i].len = args_storage[i].size();
            }

            shield_redis_value_v1* out = nullptr;
            shield_error_v1 err{};
            int rc = v.command(inst, c_args.data(),
                              static_cast<uint64_t>(c_args.size()),
                              &out, &err);
            if (rc == 0 && out) {
                results.push_back(sol::make_object(lua, true));
                results.push_back(value_to_lua(lua, out));
            } else {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua,
                    err.code ? err.code : "redis.error",
                    err.message ? err.message : "command failed"));
            }
            if (out) v.free_value(out);
            return results;
        });

    // pipeline({{cmd,...}, {cmd,...}, ...}) -> ok, {result,...}|error
    proxy.set_function("pipeline",
        [&v, inst](sol::this_state s, sol::table cmds_table) -> sol::variadic_results {
            sol::state_view lua(s);
            sol::variadic_results results;

            // Convert Lua table of tables to C pipeline commands.
            std::vector<std::vector<std::string>> cmd_strings;
            for (auto& kv : cmds_table) {
                sol::object val = kv.second;
                if (!val.is<sol::table>()) continue;
                sol::table cmd_tbl = val.as<sol::table>();
                std::vector<std::string> argv;
                for (auto& arg_kv : cmd_tbl) {
                    argv.push_back(lua_to_string(arg_kv.second));
                }
                if (!argv.empty()) {
                    cmd_strings.push_back(std::move(argv));
                }
            }

            if (cmd_strings.empty()) {
                results.push_back(sol::make_object(lua, true));
                results.push_back(sol::make_object(lua, lua.create_table()));
                return results;
            }

            // Build C arrays.
            std::vector<std::vector<shield_redis_arg_v1>> args_storage(
                cmd_strings.size());
            std::vector<shield_redis_command_v1> cmds(cmd_strings.size());
            for (size_t i = 0; i < cmd_strings.size(); ++i) {
                args_storage[i].resize(cmd_strings[i].size());
                for (size_t j = 0; j < cmd_strings[i].size(); ++j) {
                    args_storage[i][j].data = cmd_strings[i][j].data();
                    args_storage[i][j].len = cmd_strings[i][j].size();
                }
                cmds[i].args = args_storage[i].data();
                cmds[i].argc = static_cast<uint64_t>(cmd_strings[i].size());
            }

            shield_redis_value_v1* out_array = nullptr;
            uint64_t out_count = 0;
            shield_error_v1 err{};
            int rc = v.pipeline(inst, cmds.data(),
                                static_cast<uint64_t>(cmds.size()),
                                &out_array, &out_count, &err);
            if (rc == 0) {
                results.push_back(sol::make_object(lua, true));
                auto tbl = lua.create_table();
                for (uint64_t i = 0; i < out_count; ++i) {
                    tbl[i + 1] = value_to_lua(lua, &out_array[i]);
                }
                results.push_back(sol::make_object(lua, tbl));
            } else {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error_table(lua,
                    err.code ? err.code : "redis.error",
                    err.message ? err.message : "pipeline failed"));
            }
            if (out_array) {
                // Free each element, then the array.
                for (uint64_t i = 0; i < out_count; ++i) {
                    v.free_value(&out_array[i]);
                }
                std::free(out_array);
            }
            return results;
        });

    return proxy;
}

// ---------------------------------------------------------------------------
// register_lua: install shield.redis(binding) callable namespace
// ---------------------------------------------------------------------------

int register_lua_impl(shield_plugin_instance_v1* self,
                      struct lua_State* L,
                      shield_error_v1* err) {
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "redis.driver: lua_State is null";
        }
        return 1;
    }
    auto* current = reinterpret_cast<redis_instance*>(self);
    if (!current || !current->host_api ||
        !current->host_api->binding_instance_id) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "redis.driver: host binding resolver is null";
        }
        return 1;
    }
    sol::state_view lua(L);

    // Build the callable namespace shield.redis.
    auto shield = lua["shield"].get_or_create<sol::table>();

    sol::object existing = shield["redis"];
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
        shield["redis"] = ns;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// v1 ABI entry
// ---------------------------------------------------------------------------

int redis_driver_create(const shield_plugin_create_args_v1* args,
                        shield_plugin_instance_v1** out,
                        shield_error_v1* err) {
    (void)err;
    auto* inst = new redis_instance;
    inst->instance_id = args->instance_id ? args->instance_id : "";
    inst->host_api = args->host_api;
    inst->ctx = args->ctx;
    parse_instance_config(inst, args->config_json);
    register_instance(inst);

    inst->shell.struct_size = sizeof(shield_plugin_instance_v1);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = [](shield_plugin_instance_v1* self,
                                   const char* iface,
                                   shield_error_v1*) -> const void* {
        if (iface && std::strcmp(iface, SHIELD_REDIS_V1) == 0)
            return &redis_vtable();
        return nullptr;
    };
    inst->shell.start = [](shield_plugin_instance_v1* self,
                           shield_error_v1* err) -> int {
        auto* ri = reinterpret_cast<redis_instance*>(self);
        std::string open_err;
        if (!open_redis(ri, &open_err)) {
            if (err) {
                err->code = "redis.connect.failed";
                err->message = open_err.c_str();
            }
            return -1;
        }
        return 0;
    };
    inst->shell.shutdown = [](shield_plugin_instance_v1* self) {
        auto* ri = reinterpret_cast<redis_instance*>(self);
        unregister_instance(ri->instance_id);
        ri->redis.reset();
        delete ri;
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
        "redis.driver",
        "1.0.0",
        redis_driver_create,
    };
    return &abi;
}
