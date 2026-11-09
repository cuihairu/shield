// [SHIELD_PLUGIN] cache.redis — Lua facade: the instance config
// connection builder and the per-instance proxy (one fresh pooled
// connection per call), plus register_lua
// (shield.cache.redis(binding)).
// Split out of shield_cache_redis.cpp.
#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/cache.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/pool_stats.h"
#include "shield/plugin/redis.h"
#include "shield_cache_redis_impl.hpp"
#include "shield_lua_plugin_binding.hpp"

// Build a shared sw::redis::Redis from an instance config. Returns nullptr on
// failure (and fills err). Stays at global scope (as in the original) so the
// unqualified open_redis() call below sees the flat 8-arg builder declared in
// the impl header — an anonymous namespace would shadow it.
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

namespace {
// Build a Lua error table {code=..., message=...} matching the shape used by
// the host's shield.cache.* facade.
shd::table make_error_table(shd::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}
}  // namespace

namespace {
// Extract a Lua string from a shield_redis_value_v1 (STRING or NIL -> nil).
// Returns shd::nil for NIL/NULL, or a Lua string for STRING type.
static shd::object redis_value_to_lua_string(shd::state_view lua,
                                             const shield_redis_value_v1* v) {
    if (!v || v->type == SHIELD_REDIS_NIL) return shd::nil;
    if (v->type == SHIELD_REDIS_STRING && v->str) {
        return shd::make_object(
            lua, std::string(v->str, static_cast<size_t>(v->str_len)));
    }
    return shd::nil;
}
}  // namespace

namespace {
// Build a per-instance proxy table. Each method opens a redis handle, runs
// the command, then drops the shared_ptr. Return shape:
//   success: (true, ...payload...)
//   failure: (false, {code=..., message=...})
shd::table make_instance_proxy(shd::state_view lua, cache_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "get",
        [inst](shd::this_state s, std::string key) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            // redis.driver path
            if (inst->redis_driver && inst->redis_handle) {
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                int rc = inst->redis_driver->get(inst->redis_handle,
                                                 key.c_str(), &out, &err);
                if (rc == 0) {
                    results.push_back(shd::make_object(lua, true));
                    results.push_back(redis_value_to_lua_string(lua, out));
                } else {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua, err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "get failed"));
                }
                if (out) inst->redis_driver->free_value(out);
                return results;
            }
            // Fallback: internal redis++ connection
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto v = redis->get(key);
                results.push_back(shd::make_object(lua, true));
                if (v) {
                    results.push_back(shd::make_object(lua, *v));
                } else {
                    results.push_back(shd::nil);
                }
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function(
        "set",
        [inst](shd::this_state s, std::string key, std::string value,
               std::optional<lua_Integer> ttl) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                shield_error_v1 err{};
                int ttl_sec = ttl ? static_cast<int>(*ttl) : 0;
                int rc =
                    inst->redis_driver->set(inst->redis_handle, key.c_str(),
                                            value.c_str(), ttl_sec, &err);
                if (rc == 0) {
                    results.push_back(shd::make_object(lua, true));
                } else {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua, err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "set failed"));
                }
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                if (ttl && *ttl > 0)
                    redis->set(key, value, std::chrono::seconds(*ttl));
                else
                    redis->set(key, value);
                results.push_back(shd::make_object(lua, true));
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function(
        "del",
        [inst](shd::this_state s, std::string key) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                shield_error_v1 err{};
                int rc = inst->redis_driver->del(inst->redis_handle,
                                                 key.c_str(), &err);
                if (rc == 0) {
                    results.push_back(shd::make_object(lua, true));
                } else {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua, err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "del failed"));
                }
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto removed = redis->del(key);
                results.push_back(shd::make_object(lua, true));
                results.push_back(
                    shd::make_object(lua, static_cast<lua_Integer>(removed)));
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    // exists, incr, incr_by, hdel: use redis.driver raw command for operations
    // not in the typed shield.redis.v2 API.

    proxy.set_function(
        "exists",
        [inst](shd::this_state s, std::string key) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                std::string k = key;
                shield_redis_arg_v1 args[] = {{"EXISTS", 6},
                                              {k.data(), k.size()}};
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                int rc = inst->redis_driver->command(inst->redis_handle, args,
                                                     2, &out, &err);
                if (rc == 0 && out) {
                    results.push_back(shd::make_object(lua, true));
                    bool exists =
                        (out->type == SHIELD_REDIS_INTEGER && out->integer > 0);
                    results.push_back(shd::make_object(lua, exists));
                } else {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua, err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "exists failed"));
                }
                if (out) inst->redis_driver->free_value(out);
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto n = redis->exists(key);
                results.push_back(shd::make_object(lua, true));
                results.push_back(shd::make_object(lua, n > 0));
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function(
        "incr",
        [inst](shd::this_state s, std::string key) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                std::string k = key;
                shield_redis_arg_v1 args[] = {{"INCR", 4},
                                              {k.data(), k.size()}};
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                int rc = inst->redis_driver->command(inst->redis_handle, args,
                                                     2, &out, &err);
                if (rc == 0 && out && out->type == SHIELD_REDIS_INTEGER) {
                    results.push_back(shd::make_object(lua, true));
                    results.push_back(shd::make_object(
                        lua, static_cast<lua_Integer>(out->integer)));
                } else {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua,
                        (rc == 0 && out)
                            ? "cache_invalid_reply"
                            : (err.code ? err.code : "cache_query_failed"),
                        (rc == 0 && out)
                            ? "incr: expected integer reply"
                            : (err.message ? err.message : "incr failed")));
                }
                if (out) inst->redis_driver->free_value(out);
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto v = redis->incr(key);
                results.push_back(shd::make_object(lua, true));
                results.push_back(
                    shd::make_object(lua, static_cast<lua_Integer>(v)));
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function(
        "incr_by",
        [inst](shd::this_state s, std::string key,
               lua_Integer amount) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                std::string k = key;
                std::string amt =
                    std::to_string(static_cast<long long>(amount));
                shield_redis_arg_v1 args[] = {{"INCRBY", 6},
                                              {k.data(), k.size()},
                                              {amt.data(), amt.size()}};
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                int rc = inst->redis_driver->command(inst->redis_handle, args,
                                                     3, &out, &err);
                if (rc == 0 && out && out->type == SHIELD_REDIS_INTEGER) {
                    results.push_back(shd::make_object(lua, true));
                    results.push_back(shd::make_object(
                        lua, static_cast<lua_Integer>(out->integer)));
                } else {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua,
                        (rc == 0 && out)
                            ? "cache_invalid_reply"
                            : (err.code ? err.code : "cache_query_failed"),
                        (rc == 0 && out)
                            ? "incr_by: expected integer reply"
                            : (err.message ? err.message : "incr_by failed")));
                }
                if (out) inst->redis_driver->free_value(out);
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto v = redis->incrby(key, static_cast<long long>(amount));
                results.push_back(shd::make_object(lua, true));
                results.push_back(
                    shd::make_object(lua, static_cast<lua_Integer>(v)));
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function(
        "hget",
        [inst](shd::this_state s, std::string key,
               std::string field) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                int rc = inst->redis_driver->hget(
                    inst->redis_handle, key.c_str(), field.c_str(), &out, &err);
                if (rc == 0) {
                    results.push_back(shd::make_object(lua, true));
                    results.push_back(redis_value_to_lua_string(lua, out));
                } else {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua, err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "hget failed"));
                }
                if (out) inst->redis_driver->free_value(out);
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto v = redis->hget(key, field);
                results.push_back(shd::make_object(lua, true));
                if (v)
                    results.push_back(shd::make_object(lua, *v));
                else
                    results.push_back(shd::nil);
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function(
        "hset",
        [inst](shd::this_state s, std::string key, std::string field,
               std::string value) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                shield_error_v1 err{};
                int rc = inst->redis_driver->hset(inst->redis_handle,
                                                  key.c_str(), field.c_str(),
                                                  value.c_str(), &err);
                if (rc == 0) {
                    results.push_back(shd::make_object(lua, true));
                } else {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua, err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "hset failed"));
                }
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                redis->hset(key, field, value);
                results.push_back(shd::make_object(lua, true));
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function(
        "hdel",
        [inst](shd::this_state s, std::string key,
               std::string field) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            if (inst->redis_driver && inst->redis_handle) {
                std::string k = key, f = field;
                shield_redis_arg_v1 args[] = {
                    {"HDEL", 4}, {k.data(), k.size()}, {f.data(), f.size()}};
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                int rc = inst->redis_driver->command(inst->redis_handle, args,
                                                     3, &out, &err);
                if (rc == 0) {
                    results.push_back(shd::make_object(lua, true));
                    if (out && out->type == SHIELD_REDIS_INTEGER)
                        results.push_back(shd::make_object(
                            lua, static_cast<lua_Integer>(out->integer)));
                } else {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua, err.code ? err.code : "cache_query_failed",
                        err.message ? err.message : "hdel failed"));
                }
                if (out) inst->redis_driver->free_value(out);
                return results;
            }
            std::string err;
            auto redis = open_redis(inst, &err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", err));
                return results;
            }
            try {
                auto removed = redis->hdel(key, field);
                results.push_back(shd::make_object(lua, true));
                results.push_back(
                    shd::make_object(lua, static_cast<lua_Integer>(removed)));
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "cache_query_failed", e.what()));
            }
            return results;
        });

    return proxy;
}
}  // namespace

int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
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
    shd::state_view lua(L);

    // Build the callable namespace shield.cache.redis.
    shd::table shield =
        shield::plugins::get_or_create_global_subtable(lua, "shield");
    shd::table cache = shield::plugins::get_or_create_subtable(shield, "cache");

    shd::object existing = cache["redis"];
    if (!existing.is<shd::table>()) {
        auto ns = lua.create_table();
        auto mt = lua.create_table();
        const shield_host_api_v1* host_api = current->host_api;
        shield_plugin_context_v1* ctx = current->ctx;
        mt.set_function(
            "__call",
            [host_api, ctx](
                shd::this_state s, shd::table /*self*/,
                std::optional<std::string> binding) -> shd::variadic_results {
                shd::state_view lua(s);
                shd::variadic_results results;
                std::string logical = binding.value_or("");
                auto* inst = shield::plugins::resolve_lua_binding(
                    host_api, ctx, logical, find_instance);
                if (!inst) {
                    shield::plugins::push_module_unavailable(results, lua,
                                                             logical);
                    return results;
                }
                results.push_back(
                    shd::make_object(lua, make_instance_proxy(lua, inst)));
                return results;
            });
        shield::plugins::set_metatable(ns, mt);
        cache["redis"] = ns;
    }

    return 0;
}
