// [SHIELD_PLUGIN] redis.driver — Lua facade: value conversion
// helpers and the per-instance proxy whose methods reuse the same
// sw::redis::Redis pool as the C vtable, plus register_lua
// (shield.redis(binding)).
// Split out of shield_redis_driver.cpp.
#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/redis.h"
#include "shield_lua_plugin_binding.hpp"
#include "shield_redis_driver_impl.hpp"

// ---------------------------------------------------------------------------
// Lua helpers
// ---------------------------------------------------------------------------

namespace {
shd::table make_error_table(shd::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}
}  // namespace

namespace {
// Convert a shield_redis_value_v1 to a Lua object.
shd::object value_to_lua(shd::state_view lua, const shield_redis_value_v1* v) {
    if (!v) return shd::nil;
    switch (v->type) {
        case SHIELD_REDIS_NIL:
            return shd::nil;
        case SHIELD_REDIS_STRING:
            return shd::make_object(
                lua, std::string(v->str, static_cast<size_t>(v->str_len)));
        case SHIELD_REDIS_INTEGER:
            return shd::make_object(lua, static_cast<lua_Integer>(v->integer));
        case SHIELD_REDIS_DOUBLE:
            return shd::make_object(lua, v->number);
        case SHIELD_REDIS_BOOL:
            return shd::make_object(lua, v->boolean != 0);
        case SHIELD_REDIS_ARRAY: {
            auto tbl = lua.create_table();
            for (uint64_t i = 0; i < v->item_count; ++i) {
                tbl[i + 1] = value_to_lua(lua, &v->items[i]);
            }
            return shd::make_object(lua, tbl);
        }
        case SHIELD_REDIS_ERROR:
            return shd::make_object(
                lua, std::string(v->str, static_cast<size_t>(v->str_len)));
    }
    return shd::nil;
}
}  // namespace

namespace {
// Convert a Lua object to a string for command arguments.
std::string lua_to_string(shd::object obj) {
    if (obj.is<std::string>()) return obj.as<std::string>();
    if (obj.is<lua_Integer>()) return std::to_string(obj.as<lua_Integer>());
    if (obj.is<double>()) return std::to_string(obj.as<double>());
    if (obj.is<bool>()) return obj.as<bool>() ? "1" : "0";
    return "";
}
}  // namespace

namespace {
// Build a per-instance Lua proxy.
shd::table make_instance_proxy(shd::state_view lua, redis_instance* inst) {
    auto proxy = lua.create_table();
    const shield_redis_v2& v = inst->vtable_;

    // get(key) -> ok, value|nil|error
    proxy.set_function("get",
                       [&v, inst](shd::this_state s,
                                  std::string key) -> shd::variadic_results {
                           shd::state_view lua(s);
                           shd::variadic_results results;
                           shield_redis_value_v1* out = nullptr;
                           shield_error_v1 err{};
                           int rc = v.get(inst, key.c_str(), &out, &err);
                           if (rc == 0) {
                               results.push_back(shd::make_object(lua, true));
                               if (out && out->type != SHIELD_REDIS_NIL) {
                                   results.push_back(value_to_lua(lua, out));
                               } else {
                                   results.push_back(shd::nil);
                               }
                           } else {
                               results.push_back(shd::make_object(lua, false));
                               results.push_back(make_error_table(
                                   lua, err.code ? err.code : "redis.error",
                                   err.message ? err.message : "get failed"));
                           }
                           if (out) v.free_value(out);
                           return results;
                       });

    // set(key, value [, ttl]) -> ok, error
    proxy.set_function(
        "set",
        [&v, inst](shd::this_state s, std::string key, std::string value,
                   std::optional<lua_Integer> ttl) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            shield_error_v1 err{};
            int ttl_sec = ttl ? static_cast<int>(*ttl) : 0;
            int rc = v.set(inst, key.c_str(), value.c_str(), ttl_sec, &err);
            if (rc == 0) {
                results.push_back(shd::make_object(lua, true));
            } else {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, err.code ? err.code : "redis.error",
                                     err.message ? err.message : "set failed"));
            }
            return results;
        });

    // del(key) -> ok, error
    proxy.set_function("del",
                       [&v, inst](shd::this_state s,
                                  std::string key) -> shd::variadic_results {
                           shd::state_view lua(s);
                           shd::variadic_results results;
                           shield_error_v1 err{};
                           int rc = v.del(inst, key.c_str(), &err);
                           if (rc == 0) {
                               results.push_back(shd::make_object(lua, true));
                           } else {
                               results.push_back(shd::make_object(lua, false));
                               results.push_back(make_error_table(
                                   lua, err.code ? err.code : "redis.error",
                                   err.message ? err.message : "del failed"));
                           }
                           return results;
                       });

    // hget(key, field) -> ok, value|nil|error
    proxy.set_function("hget",
                       [&v, inst](shd::this_state s, std::string key,
                                  std::string field) -> shd::variadic_results {
                           shd::state_view lua(s);
                           shd::variadic_results results;
                           shield_redis_value_v1* out = nullptr;
                           shield_error_v1 err{};
                           int rc = v.hget(inst, key.c_str(), field.c_str(),
                                           &out, &err);
                           if (rc == 0) {
                               results.push_back(shd::make_object(lua, true));
                               if (out && out->type != SHIELD_REDIS_NIL) {
                                   results.push_back(value_to_lua(lua, out));
                               } else {
                                   results.push_back(shd::nil);
                               }
                           } else {
                               results.push_back(shd::make_object(lua, false));
                               results.push_back(make_error_table(
                                   lua, err.code ? err.code : "redis.error",
                                   err.message ? err.message : "hget failed"));
                           }
                           if (out) v.free_value(out);
                           return results;
                       });

    // hset(key, field, value) -> ok, error
    proxy.set_function(
        "hset",
        [&v, inst](shd::this_state s, std::string key, std::string field,
                   std::string value) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            shield_error_v1 err{};
            int rc =
                v.hset(inst, key.c_str(), field.c_str(), value.c_str(), &err);
            if (rc == 0) {
                results.push_back(shd::make_object(lua, true));
            } else {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, err.code ? err.code : "redis.error",
                    err.message ? err.message : "hset failed"));
            }
            return results;
        });

    // hgetall(key) -> ok, {field=value,...}|error
    proxy.set_function(
        "hgetall",
        [&v, inst](shd::this_state s,
                   std::string key) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            shield_redis_value_v1* out = nullptr;
            shield_error_v1 err{};
            int rc = v.hgetall(inst, key.c_str(), &out, &err);
            if (rc == 0 && out && out->type == SHIELD_REDIS_ARRAY) {
                results.push_back(shd::make_object(lua, true));
                auto tbl = lua.create_table();
                // hgetall returns alternating [field, value, field, value, ...]
                for (uint64_t i = 0; i + 1 < out->item_count; i += 2) {
                    auto& f = out->items[i];
                    auto& val = out->items[i + 1];
                    if (f.str && val.str) {
                        tbl[std::string(f.str,
                                        static_cast<size_t>(f.str_len))] =
                            std::string(val.str,
                                        static_cast<size_t>(val.str_len));
                    }
                }
                results.push_back(shd::make_object(lua, tbl));
            } else if (rc == 0) {
                results.push_back(shd::make_object(lua, true));
                results.push_back(shd::make_object(lua, lua.create_table()));
            } else {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, err.code ? err.code : "redis.error",
                    err.message ? err.message : "hgetall failed"));
            }
            if (out) v.free_value(out);
            return results;
        });

    // zadd(key, score, member) -> ok, error
    proxy.set_function(
        "zadd",
        [&v, inst](shd::this_state s, std::string key, double score,
                   std::string member) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            shield_error_v1 err{};
            int rc = v.zadd(inst, key.c_str(), score, member.c_str(), &err);
            if (rc == 0) {
                results.push_back(shd::make_object(lua, true));
            } else {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, err.code ? err.code : "redis.error",
                    err.message ? err.message : "zadd failed"));
            }
            return results;
        });

    // zrange(key, start, stop) -> ok, {member,...}|error
    proxy.set_function(
        "zrange",
        [&v, inst](shd::this_state s, std::string key, int start,
                   int stop) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            shield_redis_value_v1* out = nullptr;
            shield_error_v1 err{};
            int rc = v.zrange(inst, key.c_str(), start, stop, &out, &err);
            if (rc == 0 && out && out->type == SHIELD_REDIS_ARRAY) {
                results.push_back(shd::make_object(lua, true));
                auto tbl = lua.create_table();
                for (uint64_t i = 0; i < out->item_count; ++i) {
                    auto& item = out->items[i];
                    if (item.str) {
                        tbl[i + 1] = std::string(
                            item.str, static_cast<size_t>(item.str_len));
                    }
                }
                results.push_back(shd::make_object(lua, tbl));
            } else if (rc == 0) {
                results.push_back(shd::make_object(lua, true));
                results.push_back(shd::make_object(lua, lua.create_table()));
            } else {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, err.code ? err.code : "redis.error",
                    err.message ? err.message : "zrange failed"));
            }
            if (out) v.free_value(out);
            return results;
        });

    // command(cmd, ...) -> ok, result|error
    proxy.set_function(
        "command",
        [&v, inst](shd::this_state s,
                   shd::variadic_args va) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;

            std::vector<std::string> args_storage;
            for (auto arg : va) {
                args_storage.push_back(lua_to_string(arg));
            }
            if (args_storage.empty()) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "invalid_args",
                                     "command requires at least 1 argument"));
                return results;
            }

            std::vector<shield_redis_arg_v1> c_args(args_storage.size());
            for (size_t i = 0; i < args_storage.size(); ++i) {
                c_args[i].data = args_storage[i].data();
                c_args[i].len = args_storage[i].size();
            }

            shield_redis_value_v1* out = nullptr;
            shield_error_v1 err{};
            int rc =
                v.command(inst, c_args.data(),
                          static_cast<uint64_t>(c_args.size()), &out, &err);
            if (rc == 0 && out) {
                results.push_back(shd::make_object(lua, true));
                results.push_back(value_to_lua(lua, out));
            } else {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, err.code ? err.code : "redis.error",
                    err.message ? err.message : "command failed"));
            }
            if (out) v.free_value(out);
            return results;
        });

    // pipeline({{cmd,...}, {cmd,...}, ...}) -> ok, {result,...}|error
    proxy.set_function(
        "pipeline",
        [&v, inst](shd::this_state s,
                   shd::table cmds_table) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;

            // Convert Lua table of tables to C pipeline commands.
            std::vector<std::vector<std::string>> cmd_strings;
            for (const auto& kv : cmds_table) {
                shd::object val = kv.second;
                if (!val.is<shd::table>()) continue;
                shd::table cmd_tbl = val.as<shd::table>();
                std::vector<std::string> argv;
                for (const auto& arg_kv : cmd_tbl) {
                    argv.push_back(lua_to_string(arg_kv.second));
                }
                if (!argv.empty()) {
                    cmd_strings.push_back(std::move(argv));
                }
            }

            if (cmd_strings.empty()) {
                results.push_back(shd::make_object(lua, true));
                results.push_back(shd::make_object(lua, lua.create_table()));
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

            shield_redis_value_v1** out_array = nullptr;
            uint64_t out_count = 0;
            shield_error_v1 err{};
            int rc = v.pipeline(inst, cmds.data(),
                                static_cast<uint64_t>(cmds.size()), &out_array,
                                &out_count, &err);
            if (rc == 0) {
                results.push_back(shd::make_object(lua, true));
                auto tbl = lua.create_table();
                for (uint64_t i = 0; i < out_count; ++i) {
                    tbl[i + 1] = value_to_lua(lua, out_array[i]);
                }
                results.push_back(shd::make_object(lua, tbl));
            } else {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error_table(
                    lua, err.code ? err.code : "redis.error",
                    err.message ? err.message : "pipeline failed"));
            }
            if (out_array) {
                // Free each heap-allocated value, then the pointer array.
                for (uint64_t i = 0; i < out_count; ++i) {
                    v.free_value(out_array[i]);
                }
                std::free(out_array);
            }
            return results;
        });

    return proxy;
}
}  // namespace

// ---------------------------------------------------------------------------
// register_lua: install shield.redis(binding) callable namespace
// ---------------------------------------------------------------------------

int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
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
    shd::state_view lua(L);

    // Build the callable namespace shield.redis.
    shd::table shield =
        shield::plugins::get_or_create_global_subtable(lua, "shield");

    shd::object existing = shield["redis"];
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
        shield["redis"] = ns;
    }

    return 0;
}
