// [SHIELD_PLUGIN] leaderboard.redis — Lua facade: the per-instance
// proxy (create_board/top_n/score operations over one fresh pooled
// connection per call), board-config cache resolution and
// register_lua.
// Split out of shield_leaderboard_redis.cpp.
#include <sw/redis++/redis++.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "shield/lua/binding.hpp"
#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"
#include "shield/plugin/leaderboard.h"
#include "shield/plugin/pool_stats.h"
#include "shield/plugin/redis.h"
#include "shield_leaderboard_impl.hpp"
#include "shield_lua_plugin_binding.hpp"

namespace {
// ---------------------------------------------------------------------------
// Lua helpers.
//
// Redis connections are pooled internally by redis++; open_redis() borrows a
// fresh connection from the pool each call. Returns nullptr on failure (and
// fills an error message).
// ---------------------------------------------------------------------------

std::shared_ptr<sw::redis::Redis> open_redis(const leaderboard_instance* inst,
                                             std::string* err_msg) {
    try {
        sw::redis::ConnectionOptions opts;
        opts.host = inst->host.empty() ? "localhost" : inst->host;
        opts.port = inst->port > 0 ? inst->port : 6379;
        if (!inst->password.empty()) opts.password = inst->password;
        opts.db = inst->db > 0 ? inst->db : 0;
        opts.connect_timeout = std::chrono::milliseconds(
            inst->connect_timeout_ms > 0 ? inst->connect_timeout_ms : 5000);
        opts.socket_timeout = std::chrono::milliseconds(
            inst->command_timeout_ms > 0 ? inst->command_timeout_ms : 5000);
        auto redis = std::make_shared<sw::redis::Redis>(opts);
        redis->ping();
        return redis;
    } catch (const std::exception& e) {
        if (err_msg) *err_msg = e.what();
        return nullptr;
    }
}
}  // namespace

namespace {
// Build a Lua error table {code=..., message=...} matching the shape used by
// the host's shield.leaderboard.* facade.
shd::table make_error_table(shd::state_view lua, const char* code,
                            const std::string& msg) {
    auto t = lua.create_table();
    t["code"] = code;
    t["message"] = msg;
    return t;
}
}  // namespace

namespace {
// Extract a human-readable message from a failed redis.driver call: prefer an
// ERROR reply value, fall back to the err struct, then a static fallback.
std::string driver_err_msg(const shield_redis_value_v1* out,
                           const shield_error_v1& err, const char* fallback) {
    if (out && out->type == SHIELD_REDIS_ERROR && out->str) {
        return std::string(out->str, static_cast<size_t>(out->str_len));
    }
    if (err.message) return err.message;
    return fallback;
}
}  // namespace

// Free heap-allocated names owned by a cached BoardConfig. The instance cache
// deep-copies field names (so they outlive the Lua config table) — we must
// release them on overwrite / board delete / instance shutdown.
void free_board_config_names(BoardConfig& bc) {
    for (auto& fd : bc.field_defs) {
        fd.name.clear();
    }
}

namespace {
// Resolve a board's BoardConfig snapshot from the instance-level cache. If
// the board was never registered via create_board, returns a synthetic
// default (single "score" field, descending) — mirrors the C vtable's
// fallback behavior so the Lua and C paths agree on encoding. The returned
// BoardConfig's field_names point at string literals ("score"), not heap
// memory, so callers must NOT free them.
BoardConfig resolve_board_config(leaderboard_instance* inst,
                                 const std::string& board_name) {
    std::lock_guard<std::mutex> lock(inst->boards_mu);
    auto it = inst->boards.find(board_name);
    if (it != inst->boards.end()) return it->second;
    BoardConfig bc;
    bc.field_defs.push_back({"score", SHIELD_SORT_DESC});
    bc.field_bits.push_back(kDefaultFieldBits);
    bc.primary_desc = true;
    return bc;
}
}  // namespace

namespace {
// Build a per-instance proxy table that opens a fresh connection per call.
shd::table make_instance_proxy(shd::state_view lua,
                               leaderboard_instance* inst) {
    auto proxy = lua.create_table();

    proxy.set_function(
        "create_board",
        [inst](shd::this_state s, std::string board_name,
               shd::table def) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            // No Redis I/O needed: create_board only populates the per-instance
            // BoardConfig cache used for composite-score encoding/decoding.
            // (The C vtable's create_board is likewise a pure cache populate.)

            // Translate def = {fields={{name=,order=}, ...}, sort="asc"|"desc"}
            // into our internal BoardConfig (with heap-owned field names).
            std::vector<std::pair<std::string, shield_sort_direction>> parsed;
            shd::object fields_v = def["fields"];
            if (fields_v.is<shd::table>()) {
                shd::table fields = fields_v.as<shd::table>();
                for (const auto& kv : fields) {
                    shd::object v = kv.second;
                    if (!v.is<shd::table>()) continue;
                    shd::table fd = v.as<shd::table>();
                    std::string name = fd.get_or("name", std::string(""));
                    std::string order = fd.get_or("order", std::string("desc"));
                    if (name.empty()) continue;
                    shield_sort_direction dir =
                        (order == "asc") ? SHIELD_SORT_ASC : SHIELD_SORT_DESC;
                    parsed.emplace_back(std::move(name), dir);
                }
            }
            if (parsed.empty()) {
                // Fallback single-field board — matches vtable default.
                parsed.emplace_back("score", SHIELD_SORT_DESC);
            }
            // Top-level sort hint (asc/desc) overrides field 0's direction so
            // callers can say `sort = "asc"` at the board level for intuitive
            // top_n ordering.
            shd::object sort_v = def["sort"];
            if (sort_v.is<std::string>()) {
                const std::string top_sort = sort_v.as<std::string>();
                if (!top_sort.empty()) {
                    parsed[0].second = (top_sort == "asc") ? SHIELD_SORT_ASC
                                                           : SHIELD_SORT_DESC;
                }
            }

            // Persist into the instance-level cache. Heap-duplicate names so
            // they survive past this lambda; the instance cache frees them on
            // overwrite / board delete / shutdown.
            try {
                std::lock_guard<std::mutex> lock(inst->boards_mu);
                auto it = inst->boards.find(board_name);
                if (it != inst->boards.end()) {
                    free_board_config_names(it->second);
                }
                BoardConfig& dst = inst->boards[board_name];
                dst.field_defs.clear();
                dst.field_bits.clear();
                dst.primary_desc = (parsed[0].second == SHIELD_SORT_DESC);
                for (const auto& [name, dir] : parsed) {
                    dst.field_defs.push_back({name, dir});
                    dst.field_bits.push_back(kDefaultFieldBits);
                }
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "lb_create_failed", e.what()));
                return results;
            }
            results.push_back(shd::make_object(lua, true));
            return results;
        });

    proxy.set_function(
        "delete_board",
        [inst](shd::this_state s,
               std::string board_name) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            // Drop the instance-level cache entry so future per-call
            // queries stop using the (now-deleted) board's encoding.
            auto drop_board_cache = [inst, &board_name]() {
                std::lock_guard<std::mutex> lock(inst->boards_mu);
                auto it = inst->boards.find(board_name);
                if (it != inst->boards.end()) {
                    free_board_config_names(it->second);
                    inst->boards.erase(it);
                }
            };
            // Driver path: DEL through redis.driver's typed del.
            if (inst->redis_driver && inst->redis_handle) {
                shield_error_v1 err{};
                const int rc = inst->redis_driver->del(
                    inst->redis_handle, board_name.c_str(), &err);
                if (rc != 0) {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua, err.code ? err.code : "lb_query_failed",
                        err.message ? err.message : "del failed"));
                    return results;
                }
                drop_board_cache();
                results.push_back(shd::make_object(lua, true));
                return results;
            }
            std::string open_err;
            auto redis = open_redis(inst, &open_err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }
            try {
                redis->del(board_name);
                drop_board_cache();
                results.push_back(shd::make_object(lua, true));
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "lb_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function(
        "set_entry",
        [inst](shd::this_state s, std::string board_name, std::string player_id,
               shd::table fields_table) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            // Translate the Lua fields table {field_name = number, ...} into
            // parallel C arrays. encode_composite looks up each field by name,
            // so caller-supplied order does not matter.
            std::vector<std::string> names_storage;
            std::vector<double> values_storage;
            for (const auto& kv : fields_table) {
                shd::object key = kv.first;
                shd::object val = kv.second;
                if (key.get_type() != shd::type::string) continue;
                if (!val.is<double>() && !val.is<lua_Integer>() &&
                    !val.is<bool>())
                    continue;
                names_storage.push_back(key.as<std::string>());
                if (val.is<bool>()) {
                    values_storage.push_back(val.as<bool>() ? 1.0 : 0.0);
                } else if (val.is<lua_Integer>()) {
                    values_storage.push_back(
                        static_cast<double>(val.as<lua_Integer>()));
                } else {
                    values_storage.push_back(val.as<double>());
                }
            }
            std::vector<const char*> names_ptr(names_storage.size(), nullptr);
            for (size_t i = 0; i < names_storage.size(); ++i) {
                names_ptr[i] = names_storage[i].c_str();
            }
            // Resolve the board's BoardConfig from the per-instance cache and
            // encode via the SAME encode_composite() the C vtable uses.
            BoardConfig bc = resolve_board_config(inst, board_name);
            double score =
                encode_composite(bc, names_ptr.data(), values_storage.data(),
                                 static_cast<int>(names_storage.size()));
            // Driver path: ZADD through redis.driver's typed zadd.
            if (inst->redis_driver && inst->redis_handle) {
                shield_error_v1 err{};
                const int rc = inst->redis_driver->zadd(
                    inst->redis_handle, board_name.c_str(), score,
                    player_id.c_str(), &err);
                if (rc != 0) {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(
                        lua, err.code ? err.code : "lb_query_failed",
                        err.message ? err.message : "zadd failed"));
                    return results;
                }
                results.push_back(shd::make_object(lua, true));
                return results;
            }
            std::string open_err;
            auto redis = open_redis(inst, &open_err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }
            try {
                redis->zadd(board_name, player_id, score);
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "lb_query_failed", e.what()));
                return results;
            }
            results.push_back(shd::make_object(lua, true));
            return results;
        });

    proxy.set_function(
        "remove_entry",
        [inst](shd::this_state s, std::string board_name,
               std::string player_id) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            // Driver path: ZREM via the raw command escape hatch.
            if (inst->redis_driver && inst->redis_handle) {
                const shield_redis_arg_v1 args[] = {
                    {"ZREM", 4},
                    {board_name.data(), board_name.size()},
                    {player_id.data(), player_id.size()},
                };
                shield_redis_value_v1* out = nullptr;
                shield_error_v1 err{};
                const int rc = inst->redis_driver->command(inst->redis_handle,
                                                           args, 3, &out, &err);
                const bool ok =
                    rc == 0 && !(out && out->type == SHIELD_REDIS_ERROR);
                const std::string emsg =
                    ok ? std::string()
                       : driver_err_msg(out, err, "zrem failed");
                if (out) inst->redis_driver->free_value(out);
                if (!ok) {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(
                        make_error_table(lua, "lb_query_failed", emsg));
                    return results;
                }
                results.push_back(shd::make_object(lua, true));
                return results;
            }
            std::string open_err;
            auto redis = open_redis(inst, &open_err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }
            try {
                redis->zrem(board_name, player_id);
                results.push_back(shd::make_object(lua, true));
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "lb_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function(
        "get_entry",
        [inst](shd::this_state s, std::string board_name,
               std::string player_id) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string open_err;
            auto redis = open_redis(inst, &open_err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }
            try {
                auto score_opt = redis->zscore(board_name, player_id);
                if (!score_opt) {
                    // Player not present. (true, nil) so callers can
                    // distinguish "board ok, no entry" from a hard failure.
                    results.push_back(shd::make_object(lua, true));
                    results.push_back(shd::make_object(lua, shd::nil));
                    return results;
                }
                BoardConfig bc = resolve_board_config(inst, board_name);
                std::vector<double> decoded;
                decode_composite(bc, *score_opt, decoded);
                auto entry_table = lua.create_table();
                entry_table["player_id"] = player_id;
                auto fields = lua.create_table();
                for (size_t i = 0;
                     i < bc.field_defs.size() && i < decoded.size(); ++i) {
                    if (!bc.field_defs[i].name.empty()) {
                        fields[bc.field_defs[i].name] = decoded[i];
                    }
                }
                entry_table["fields"] = fields;
                results.push_back(shd::make_object(lua, true));
                results.push_back(entry_table);
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "lb_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function(
        "get_rank",
        [inst](shd::this_state s, std::string board_name,
               std::string player_id) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string open_err;
            auto redis = open_redis(inst, &open_err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }
            try {
                auto r = redis->zrank(board_name, player_id);
                if (!r) {
                    // Not ranked — ok=true with rank=0 (matches vtable).
                    results.push_back(shd::make_object(lua, true));
                    results.push_back(
                        shd::make_object(lua, static_cast<lua_Integer>(0)));
                } else {
                    results.push_back(shd::make_object(lua, true));
                    results.push_back(shd::make_object(
                        lua, static_cast<lua_Integer>(*r + 1)));
                }
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "lb_query_failed", e.what()));
            }
            return results;
        });

    proxy.set_function(
        "top_n",
        [inst](shd::this_state s, std::string board_name,
               int n) -> shd::variadic_results {
            shd::state_view lua(s);
            shd::variadic_results results;
            std::string open_err;
            auto redis = open_redis(inst, &open_err);
            if (!redis) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "connection_failed", open_err));
                return results;
            }
            // The vtable's top_n returns only player_id + rank (it drops the
            // ZSET score), which would force Lua callers to re-query per
            // player to recover field values. For the Lua path we go directly
            // to Redis (ZRANGE WITHSCORES) and decode the composite score here
            // using the same decode_composite() the vtable uses, so callers
            // get the full per-field snapshot in one call.
            try {
                if (n <= 0) {
                    results.push_back(shd::make_object(lua, false));
                    results.push_back(make_error_table(lua, "invalid_args",
                                                       "n must be positive"));
                    return results;
                }
                BoardConfig bc = resolve_board_config(inst, board_name);
                std::vector<std::pair<std::string, double>> rows;
                if (bc.primary_desc) {
                    redis->zrevrange(board_name, 0, n - 1,
                                     std::back_inserter(rows));
                } else {
                    redis->zrange(board_name, 0, n - 1,
                                  std::back_inserter(rows));
                }
                auto entries = lua.create_table();
                int idx = 1;
                for (const auto& [member, score] : rows) {
                    auto row = lua.create_table();
                    row["player_id"] = member;
                    row["rank"] = static_cast<lua_Integer>(idx);
                    auto fields = lua.create_table();
                    std::vector<double> decoded;
                    decode_composite(bc, score, decoded);
                    for (size_t i = 0;
                         i < bc.field_defs.size() && i < decoded.size(); ++i) {
                        if (!bc.field_defs[i].name.empty()) {
                            fields[bc.field_defs[i].name] = decoded[i];
                        }
                    }
                    row["fields"] = fields;
                    entries[idx++] = row;
                }
                results.push_back(shd::make_object(lua, true));
                results.push_back(entries);
                return results;
            } catch (const std::exception& e) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error_table(lua, "lb_query_failed", e.what()));
                return results;
            }
        });

    return proxy;
}
}  // namespace

int register_lua_impl(shield_plugin_instance_v1* self, struct lua_State* L,
                      shield_error_v1* err) {
    // register_lua installs the shared, idempotent callable namespace
    // shield.leaderboard.redis. Lua passes a binding logical name; host config
    // resolves that binding to the deployment instance id.
    if (!L) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "leaderboard.redis: lua_State is null";
        }
        return 1;
    }
    auto* current = reinterpret_cast<leaderboard_instance*>(self);
    if (!current || !current->host_api ||
        !current->host_api->binding_instance_id) {
        if (err) {
            err->code = "plugin.lua_register.failed";
            err->message = "leaderboard.redis: host binding resolver is null";
        }
        return 1;
    }
    shd::state_view lua(L);

    // Build the callable namespace shield.leaderboard.redis.
    shd::table shield =
        shield::plugins::get_or_create_global_subtable(lua, "shield");
    shd::table leaderboard =
        shield::plugins::get_or_create_subtable(shield, "leaderboard");

    shd::object existing = leaderboard["redis"];
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
        leaderboard["redis"] = ns;
    }

    return 0;
}
