// [SHIELD_LUA] Lua API registration
#include "shield/lua/lua_api.hpp"

#include "shield/config/config.hpp"
#ifdef SHIELD_ENABLE_CLUSTER
#include "shield/cluster/cluster_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_PLAYER
#include "shield/player/player_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_SERVER
#include "shield/server/server_manager.hpp"
#endif
#ifdef SHIELD_ENABLE_GLOBAL
#include "shield/global/global_manager.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <caf/actor.hpp>
#include <caf/send.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shield/core/service_message.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/client_identity.hpp"
#include "shield/lua/lua_constants.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/lua/player_ref_box.hpp"
#include "shield/net/http_client.hpp"
#include "shield/net/session.hpp"
#include "shield/plugin/plugin_host.hpp"

namespace shield::lua {

sol::table make_error(sol::this_state state, std::string code,
                      std::string message, bool retryable = false,
                      sol::object detail = sol::nil) {
    sol::state_view lua(state);
    sol::table err = lua.create_table();
    err["code"] = std::move(code);
    err["message"] = std::move(message);
    err["retryable"] = retryable;
    if (detail.valid() && detail != sol::nil) {
        err["detail"] = detail;  // GCOVR_EXCL_LINE (no coverage-suite caller
                                 // passes a detail object)
    }
    return err;
}

// Map a raw call-failure message to the stable call error code (shared by
// the coroutine call wrapper's string-shaping and the _coro_call pre-dispatch
// failures).
std::string call_error_code_for(const std::string& msg) {
    if (msg.find("service not found") != std::string::npos)
        return "service_not_found";
    if (msg.find("service dead") != std::string::npos) return "service_dead";
    if (msg.find("method not found") != std::string::npos)
        return "method_not_found";
    if (msg.find("runtime is stopping") != std::string::npos)
        return "runtime_stopping";
    if (msg.find("invalid method") != std::string::npos)
        return "invalid_method";
    if (msg.find("coroutine limit") != std::string::npos)
        return "coroutine_limit";
    return "handler_error";
}

// -- Client identity userdata (ClientContext / ClientRef) ---------------------
//
// Both wrap the same trusted identity snapshot (ClientContextData, see
// client_identity.hpp). A ClientContext materializes whenever a
// __shield_client_ref marker arrives in a message payload (gateway ingress,
// on_connect/on_disconnect); a ClientRef is what shield.client.bind returns.
// Lua cannot construct them (sol::no_constructor) and cannot mutate them.
// Passing either as a message argument serializes back to the marker form.

namespace {

// Shared read-only property binding for ClientContext and ClientRef: the
// concrete Box parameter keeps sol's wrapper happy (a generic lambda is not
// convertible to a single function pointer).
template <typename Box>
void bind_identity_properties(sol::usertype<Box>& type) {
    type.set("player_id", [](const Box& box) { return box.data.player_id; });
    type.set("session_id", [](const Box& box) { return box.data.session_id; });
    type.set("session_epoch",      // GCOVR_EXCL_LINE (gcov clone artifact)
             [](const Box& box) {  // GCOVR_EXCL_LINE
                 return box.data.session_epoch;  // GCOVR_EXCL_LINE
             });
    type.set("protocol_profile_id",  // GCOVR_EXCL_LINE (gcov clone artifact)
             [](const Box& box) {    // GCOVR_EXCL_LINE
                 return box.data.protocol_profile_id;  // GCOVR_EXCL_LINE
             });
    type.set("gateway",
             [](const Box& box) { return box.data.gateway_address; });
}

bool extract_client_data(const sol::object& object, ClientContextData* out) {
    if (object.is<ClientContextBox>()) {
        *out = object.as<const ClientContextBox&>().data;
        return true;
    }
    if (object.is<ClientRefBox>()) {
        *out = object.as<const ClientRefBox&>().data;
        return true;
    }
    return false;
}

}  // namespace

// Reads the client identity out of a ClientContext/ClientRef argument (or a
// __shield_client_ref marker table that travelled through a path without
// materialization). Used by shield.client.bind/close and the client_rpc
// egress helpers.
static bool client_arg_to_data(const sol::object& object,
                               ClientContextData* out) {
    if (extract_client_data(object, out)) {
        return true;
    }
    if (object.is<sol::table>()) {
        auto data = ClientContextData::from_json(lua_to_json(object));
        if (data.has_value()) {
            *out = std::move(*data);
            return true;
        }
    }
    return false;
}

sol::object json_to_lua(sol::state_view lua, const nlohmann::json& value) {
    if (value.is_null()) {
        return sol::make_object(lua, sol::nil);
    }
    if (value.is_boolean()) {
        return sol::make_object(lua, value.get<bool>());
    }
    if (value.is_number_integer()) {
        return sol::make_object(lua, value.get<std::int64_t>());
    }
    // GCOVR_EXCL_START (unreachable: is_number_integer() also matches
    // unsigned values, so this arm can never be selected)
    if (value.is_number_unsigned()) {
        return sol::make_object(lua, value.get<std::uint64_t>());
    }
    // GCOVR_EXCL_STOP
    if (value.is_number_float()) {
        return sol::make_object(lua, value.get<double>());
    }
    if (value.is_string()) {
        return sol::make_object(lua, value.get<std::string>());
    }
    if (value.is_array()) {
        sol::table table = lua.create_table();
        int index = 1;
        for (const auto& item : value) {
            table[index++] = json_to_lua(lua, item);
        }
        return sol::make_object(lua, table);
    }
    if (value.is_object()) {
#ifdef SHIELD_ENABLE_PLAYER
        // A __shield_player_ref marker materializes as the read-only
        // PlayerRef userdata (value semantics, no mailbox). Missing fields
        // degrade to defaults, mirroring the client-identity handling.
        if (value.contains("__shield_player_ref") &&
            value.value("__shield_player_ref", false) == true) {
            PlayerRefData ref;
            if (auto it = value.find("uid");
                it != value.end() && it->is_string()) {
                ref.uid = it->get<std::string>();
            }
            if (auto it = value.find("node_id");
                it != value.end() && it->is_string()) {
                ref.node_id = it->get<std::string>();
            }
            if (auto it = value.find("service_id");
                it != value.end() && it->is_string()) {
                ref.service_id = it->get<std::string>();
            }
            // A JSON literal like `5` decodes as a signed integer; accept
            // both integer flavors so marker epochs round-trip.
            if (auto it = value.find("epoch"); it != value.end() &&
                                               it->is_number_integer() &&
                                               it->get<std::int64_t>() >= 0) {
                ref.epoch = it->get<std::uint64_t>();
            }
            return sol::make_object(lua, PlayerRefBox{std::move(ref)});
        }
#endif
        // A trusted client-identity marker materializes as the read-only
        // ClientContext userdata. from_json does all field validation, so a
        // malformed field degrades to its default instead of throwing.
        if (auto ctx = ClientContextData::from_json(value)) {
            sol::object maybe_ud = lua["__shield_make_client_context"];
            if (maybe_ud.valid() && maybe_ud.is<sol::protected_function>()) {
                sol::protected_function make_context =
                    maybe_ud.as<sol::protected_function>();
                auto result = make_context(ctx->session_id, ctx->session_epoch,
                                           ctx->player_id, ctx->gateway_address,
                                           ctx->protocol_profile_id);
                if (result.valid() && result.return_count() > 0) {
                    return result.get<sol::object>(0);
                }
            }
        }
        sol::table table = lua.create_table();
        for (const auto& [key, item] : value.items()) {
            table[key] = json_to_lua(lua, item);
        }
        return sol::make_object(lua, table);
    }
    return sol::make_object(lua, sol::nil);
}

nlohmann::json lua_table_to_json(const sol::table& table) {
    bool array_like = true;
    std::size_t max_index = 0;
    std::size_t entry_count = 0;

    for (const auto& [key, _] : table) {
        ++entry_count;
        sol::object key_obj = key;
        if (!key_obj.is<int>()) {
            array_like = false;
            break;
        }

        const int index = key_obj.as<int>();
        if (index <= 0) {
            array_like = false;
            break;
        }
        max_index = std::max(max_index, static_cast<std::size_t>(index));
    }

    if (array_like && max_index == entry_count) {
        nlohmann::json array = nlohmann::json::array();
        for (std::size_t i = 1; i <= max_index; ++i) {
            array.push_back(lua_to_json(table[static_cast<int>(i)]));
        }
        return array;
    }  // GCOVR_EXCL_LINE

    nlohmann::json object = nlohmann::json::object();
    for (const auto& [key, value] : table) {
        sol::object key_obj = key;
        std::string object_key;
        if (key_obj.is<std::string>()) {
            object_key = key_obj.as<std::string>();
        } else if (key_obj.is<int>()) {
            object_key = std::to_string(key_obj.as<int>());
        } else {
            continue;
        }
        object[object_key] = lua_to_json(value);
    }
    return object;
}

nlohmann::json variadic_to_json_array(sol::variadic_args args) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& arg : args) {
        values.push_back(lua_to_json(arg));
    }
    return values;
}  // GCOVR_EXCL_LINE

// Helper to extract service ID from ServiceHandle or string
std::string extract_service_id(const sol::object& target) {
    if (target.is<ServiceHandle>()) {
        return target.as<ServiceHandle>().id();
    }
    if (target.is<std::string>()) {
        return target.as<std::string>();
    }
    return "";
}

void register_service_api(sol::table& shield, LuaServiceManager* manager) {
    // Synchronous spawn primitive: runs VM creation + on_init on the calling
    // thread. The public shield.spawn wrapper (below) uses this on the main
    // thread, inside a spawn's on_init, and as the fallback when the
    // coroutine path cannot suspend.
    shield.set_function(
        "_sync_spawn",
        [manager](sol::this_state state, std::string module,
                  sol::optional<sol::table> opts) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;

            nlohmann::json options =
                opts ? lua_table_to_json(*opts) : nlohmann::json::object();
            if (!options.is_object()) {
                options = nlohmann::json::object();
            }

            SpawnResult result = manager->spawn(module, options.dump());
            if (!result.success) {
                std::string code = "spawn_failed";
                if (result.error_message.find("timeout") != std::string::npos) {
                    code = "spawn_timeout";
                } else if (result.error_message.find("on_init failed") !=
                           std::string::npos) {
                    code = "init_failed";
                }
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(
                    make_error(state, std::move(code), result.error_message));
                return results;
            }

            // Return ServiceHandle userdata instead of string
            ServiceHandle handle(result.service_id);
            results.push_back(sol::make_object(lua, handle));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    shield.set_function("exit", [manager](sol::optional<std::string> reason) {
        manager->request_current_exit(reason.value_or("normal"));
    });

    // True while a spawn's on_init runs its initial segment on the spawning
    // thread: shield.spawn resolves synchronously there (the child's init
    // blocks the spawner, never a service actor).
    shield.set_function("_in_on_init", [] {
        return LuaServiceManager::spawn_init_in_progress();
    });

    shield.set_function(
        "self", [manager](sol::this_state state) -> sol::object {
            sol::state_view lua(state);
            const auto service_id = manager->current_service_id();
            if (service_id.empty()) {
                return sol::make_object(lua, sol::nil);
            }
            ServiceHandle handle(service_id);
            return sol::make_object(lua, handle);
        });

    shield.set_function("names",
                        [manager](sol::this_state state) -> sol::table {
                            sol::state_view lua(state);
                            sol::table names = lua.create_table();

                            int index = 1;
                            for (const auto& name : manager->list_services()) {
                                names[index++] = name;
                            }
                            return names;
                        });

    shield.set_function(
        "query",
        [manager](sol::this_state state,
                  std::string name) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;

            const auto service = manager->query_service(name);
            if (!service.empty()) {
                ServiceHandle handle(service);
                results.push_back(sol::make_object(lua, handle));
                results.push_back(sol::make_object(lua, sol::nil));
                return results;
            }

            results.push_back(sol::make_object(lua, sol::nil));
            results.push_back(make_error(state, "service_not_found",
                                         "service not found: " + name));
            return results;
        });

    shield.set_function(
        "register",
        [manager](sol::this_state state,
                  std::string name) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;

            std::string error;
            if (!manager->register_name(name, &error)) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(state, "register_failed", error));
                return results;
            }

            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    shield.set_function("unregister",
                        [manager](sol::this_state state,
                                  std::string name) -> sol::variadic_results {
                            sol::state_view lua(state);
                            sol::variadic_results results;

                            std::string error;
                            if (!manager->unregister_name(name, &error)) {
                                results.push_back(sol::make_object(lua, false));
                                results.push_back(make_error(
                                    state, "unregister_failed", error));
                                return results;
                            }

                            results.push_back(sol::make_object(lua, true));
                            results.push_back(sol::make_object(lua, sol::nil));
                            return results;
                        });

    // Coroutine-aware spawn primitive. Suspends the caller's coroutine and
    // queues the blocking part (VM creation + module load + on_init) onto the
    // manager's spawn worker thread; the caller is resumed with
    // [true, service_id] or [false, error_table] when the spawn completes
    // (or on timeout). Returns the session id, or 0 when the coroutine path
    // is unavailable (no dispatch context / runtime stopping) and the caller
    // must fall back to _sync_spawn.
    shield.set_function(
        "_coro_spawn",
        [manager](sol::this_state state, std::string module,
                  sol::optional<sol::table> opts, int timeout_ms) -> uint64_t {
            if (manager->current_service_id().empty()) {
                return 0;
            }
            nlohmann::json options =
                opts ? lua_table_to_json(*opts) : nlohmann::json::object();
            if (!options.is_object()) {
                options = nlohmann::json::object();
            }

            lua_State* co = state;
            const uint64_t session = manager->suspend_for_call(co, timeout_ms);
            if (!manager->enqueue_async_spawn(
                    session, std::move(module),
                    options.dump())) {  // GCOVR_EXCL_START (defensive: stopping
                                        // race, enqueue fails only at shutdown)
                nlohmann::json err = "runtime is stopping";
                manager->resume_caller(session, false,
                                       nlohmann::json::array({err}));
                return 0;
            }
            // GCOVR_EXCL_STOP
            return session;
        });

    // Rebuild a ServiceHandle userdata from a service id. Used by the
    // shield.spawn wrapper after a coroutine resume (the response channel
    // carries JSON, not userdata).
    shield.set_function(
        "_make_handle",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](sol::this_state state,  // GCOVR_EXCL_LINE
           std::string service_id) -> sol::object {
            sol::state_view lua(state);
            ServiceHandle handle(std::move(service_id));
            return sol::make_object(lua, handle);
        });

    // Business-triggered panic: invoke on_panic(reason, {type="explicit"})
    // and exit the current service with reason "panic".
    shield.set_function("panic", [manager](sol::optional<std::string> reason) {
        manager->panic_current(reason.value_or("explicit panic"));
    });

    // Public shield.spawn: suspend inside handler coroutines (the spawn
    // worker runs on_init off-actor), stay synchronous on the main thread and
    // inside a spawn's on_init (on the spawning thread).
    sol::state_view lua(shield.lua_state());
    lua["shield"] = shield;
    lua.safe_script(
        "shield.spawn = function(module, opts)\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain or shield._in_on_init() then return "
        "shield._sync_spawn(module, opts) end\n"
        "  local timeout = (type(opts) == 'table' and opts.timeout) or 10000\n"
        "  local session = shield._coro_spawn(module, opts, timeout)\n"
        "  if session == 0 then return shield._sync_spawn(module, opts) end\n"
        "  local r = table.pack(coroutine.yield())\n"
        "  if not r[1] then\n"
        "    if type(r[2]) == 'table' and r[2].code == 'timeout' then\n"
        "      r[2].code = 'spawn_timeout'\n"
        "      r[2].message = 'spawn timeout'\n"
        "    end\n"
        "    return nil, r[2]\n"
        "  end\n"
        "  return shield._make_handle(r[2]), nil\n"
        "end\n",
        // The two header lines below are a gcov artifact: the identity adapter
        // body runs (its closing brace is covered) but the opening arc is
        // emitted only into an outlined clone that is never called.
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::protected_function_result  // GCOVR_EXCL_LINE (gcov clone
                                           // artifact)
               pfr)                        // GCOVR_EXCL_LINE (gcov
                                           // clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
}

#ifdef SHIELD_ENABLE_CLUSTER
namespace {

// Remote-target resolution for shield.send/call (M4). Local names always
// win: "room.public" resolves locally even if a remote route shares the
// name, and a colon-name that hits locally is never reinterpreted as
// "node:service".
struct RemoteResolution {
    bool is_remote = false;
    // Meaningful when is_remote: envelope destination.
    std::string node;
    std::string service_id;
    // Non-empty when the remote target failed pre-flight (reachability /
    // route lookup): the caller fails the send/call with this stable code
    // instead of dispatching.
    std::string error_code;
    std::string error_message;
};

// Node-level failures recover on reconnect: surface them as retryable.
bool remote_error_retryable(const std::string& code) {
    return code == "node_offline" || code == "node_suspect";
}

// Stable code for a send_remote failure string (e.g. the transport's
// "node_offline" out-param).
std::string remote_send_error_code(const std::string& msg) {
    if (msg.find("node_suspect") != std::string::npos) return "node_suspect";
    if (msg.find("node_offline") != std::string::npos) return "node_offline";
    return "transport_failed";
}

RemoteResolution resolve_remote_target(LuaServiceManager* manager,
                                       const std::string& target_id) {
    RemoteResolution res;
    // Local hit wins, never reinterpreted as "node:service".
    if (!manager->query_service(target_id).empty()) {
        return res;
    }
    std::string node, name;
    if (!shield::cluster::ClusterManager::parse_remote_target(target_id, node,
                                                              name)) {
        return res;
    }
    auto* cm = shield::cluster::global_cluster_manager();
    if (!cm) {
        // Cluster not initialized: no node namespace exists; treat as an
        // ordinary local miss so the local path produces service_not_found.
        return res;  // GCOVR_EXCL_LINE (no cluster-less suite sends a
    }  // qualified target; the local-miss contract is covered unqualified)
    if (node == cm->node_id()) {
        // Self-qualified name: plain local service (already missed above).
        return res;
    }
    res.is_remote = true;
    res.node = node;
    if (const std::string reachable = cm->check_node_reachable(node);
        !reachable.empty()) {
        res.error_code = reachable;
        res.error_message = "cluster node is not reachable: " + node;
        return res;
    }
    res.service_id = cm->query_remote(node, name);
    if (res.service_id.empty()) {
        res.error_code = "service_not_found";
        res.error_message = "remote service not found: " + name + " on " + node;
    }
    return res;
}

}  // namespace
#endif

void register_message_api(sol::table& shield, LuaServiceManager* manager,
                          LuaRuntime* runtime) {
    shield.set_function(
        "send",
        [manager](sol::this_state state, sol::object target, std::string method,
                  sol::variadic_args args) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;

            std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                results.push_back(sol::make_object(lua, false));
                results.push_back(
                    make_error(state, "invalid_target",
                               "target must be ServiceHandle or string"));
                return results;
            }

#ifdef SHIELD_ENABLE_CLUSTER
            // Cross-node fire-and-forget (M4): "node:service" leaves as an
            // envelope; local names keep the in-process path below.
            const auto remote = resolve_remote_target(manager, target_id);
            if (remote.is_remote) {
                if (!remote.error_code.empty()) {
                    results.push_back(sol::make_object(lua, false));
                    results.push_back(make_error(
                        state, remote.error_code, remote.error_message,
                        remote_error_retryable(remote.error_code)));
                    return results;
                }
                std::string send_error;
                auto* cm = shield::cluster::global_cluster_manager();
                if (cm->send_remote(remote.node, remote.service_id, method,
                                    variadic_to_json_array(args).dump(), 0, 0,
                                    &send_error)) {
                    results.push_back(sol::make_object(lua, true));
                    results.push_back(sol::make_object(lua, sol::nil));
                    return results;
                }
                results.push_back(sol::make_object(lua, false));
                results.push_back(make_error(
                    state, remote_send_error_code(send_error), send_error,
                    remote_error_retryable(
                        remote_send_error_code(send_error))));
                return results;
            }
#endif

            std::string error;
            if (!manager->send(target_id, method, variadic_to_json_array(args),
                               &error)) {
                // Map error message to stable error code.
                std::string code = "service_not_found";
                bool retryable = false;
                if (error.find("runtime is stopping") != std::string::npos) {
                    code = "runtime_stopping";
                } else if (error.find("message too large") !=
                           std::string::npos) {
                    code = "message_too_large";
                } else if (error.find("unsupported") != std::string::npos) {
                    code = "encode_failed";
                } else if (error.find(
                               "permission denied") !=  // GCOVR_EXCL_LINE
                           std::string::npos) {
                    code = "permission_denied";  // GCOVR_EXCL_LINE
                } else if (error.find("invalid method") != std::string::npos) {
                    code = "invalid_method";
                } else if (error.find("service dead") != std::string::npos) {
                    code = "service_dead";
                } else if (error.find("coroutine limit") !=  // GCOVR_EXCL_LINE
                           std::string::npos) {
                    code = "coroutine_limit";  // GCOVR_EXCL_LINE
                }
                results.push_back(sol::make_object(lua, false));
                results.push_back(
                    make_error(state, std::move(code), error, retryable));
                return results;
            }

            results.push_back(sol::make_object(lua, true));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    // Coroutine-aware call primitive. Suspends the caller's coroutine and
    // sends a call-request message to the target; the caller is resumed with
    // [ok, values...] when the callee completes (or on timeout). Returns the
    // session id (0 if the caller is not inside a coroutine).
    shield.set_function(
        "_coro_call",
        [manager](sol::this_state state, sol::object target, std::string method,
                  sol::table args, int timeout_ms) -> uint64_t {
            // Refuse to suspend the main thread: the Lua wrapper never
            // dispatches off-coroutine, and anchoring the main thread here
            // would leave it suspended with no resume source.
            if (lua_pushthread(state) == 1) {
                lua_pop(state, 1);
                return 0;
            }
            lua_pop(state, 1);
            const std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                // Invalid target shape: suspend, then complete asynchronously
                // so the yielded wrapper gets the stable error table.
                lua_State* co = state;  // GCOVR_EXCL_LINE (continuation)
                const uint64_t session =
                    manager->suspend_for_call(co,  // GCOVR_EXCL_LINE
                                              timeout_ms);
                manager->complete_call(  // GCOVR_EXCL_LINE (continuation)
                    session, false,
                    nlohmann::json::array(  // GCOVR_EXCL_LINE (continuation)
                        {nlohmann::json::object(  // GCOVR_EXCL_LINE
                                                  // (continuation)
                            {{"code", "invalid_target"},
                             {"message",
                              "target must be ServiceHandle or string"},
                             {"retryable", false}})}));
                return session;  // GCOVR_EXCL_LINE
            }

            // Pack the arguments once for both dispatch paths.
            std::size_t arg_count = args.size();
            sol::object packed_count = args["n"];
            if (packed_count.valid() && packed_count.is<int>()) {
                const int n = packed_count.as<int>();
                arg_count = n > 0 ? static_cast<std::size_t>(n) : 0;
            }
            nlohmann::json json_args = nlohmann::json::array();
            for (std::size_t i = 1; i <= arg_count; ++i) {
                json_args.push_back(lua_to_json(args[static_cast<int>(i)]));
            }

#ifdef SHIELD_ENABLE_CLUSTER
            // Cross-node call (M4): suspend first, then pre-flight and send;
            // every failure completes the session asynchronously (response
            // message lands after the caller yields) so the wrapper's
            // coroutine.yield() always gets exactly one resume.
            const auto remote = resolve_remote_target(manager, target_id);
            if (remote.is_remote) {
                lua_State* co = state;
                const uint64_t session =
                    manager->suspend_for_call(co, timeout_ms);
                if (!remote.error_code.empty()) {
                    manager->complete_call(
                        session, false,
                        nlohmann::json::array({nlohmann::json::object(
                            {{"code", remote.error_code},
                             {"message", remote.error_message},
                             {"retryable",             // GCOVR_EXCL_LINE
                              remote_error_retryable(  // GCOVR_EXCL_LINE
                                  remote.error_code)}})}));
                    // GCOVR_EXCL_LINE (continuation)
                    return session;
                }
                std::string send_error;
                auto* cm = shield::cluster::global_cluster_manager();
                if (!cm->send_remote(remote.node, remote.service_id, method,
                                     json_args.dump(), session, timeout_ms,
                                     &send_error)) {
                    const std::string code = remote_send_error_code(send_error);
                    manager->complete_call(
                        session, false,
                        nlohmann::json::array({nlohmann::json::object(
                            {{"code", code},
                             {"message",  // GCOVR_EXCL_LINE (continuation)
                              send_error},
                             {"retryable",             // GCOVR_EXCL_LINE
                              remote_error_retryable(  // GCOVR_EXCL_LINE
                                                       // (continuation)
                                  code)}})}));         // GCOVR_EXCL_LINE
                                                       // (continuation)
                }  // GCOVR_EXCL_LINE (arc artifact of the excluded error
                   // branch)
                // Non-zero session: the wrapper yields and the completion
                // (success or the failure above) resumes it.
                return session;
            }
#endif

            const std::string service_id = manager->query_service(target_id);
            if (service_id.empty()) {
                // Unknown local target: suspend, then complete asynchronously
                // (via the caller's own actor mailbox) so the wrapper's
                // coroutine.yield() still gets exactly one resume with the
                // stable error table.
                lua_State* co = state;
                const uint64_t session =
                    manager->suspend_for_call(co, timeout_ms);
                manager->complete_call(
                    session, false,
                    nlohmann::json::array({nlohmann::json::object(
                        {{"code", "service_not_found"},
                         {"message", "service not found: " + target_id},
                         {"retryable", false}})}));
                return session;
            }
            lua_State* co = state;
            const uint64_t session = manager->suspend_for_call(co, timeout_ms);

            // Build and queue the call-request message.
            std::string send_error;
            // Send carries call_session so the callee's dispatch can route the
            // response back to the caller.
            if (!manager->send_call_request(service_id, method, json_args,
                                            session, &send_error)) {
                // Could not queue (e.g. runtime stopping): complete the
                // session through the same async channel so the caller
                // resumes with a stable error table.
                // GCOVR_EXCL_START (race window: the target's actor is gone
                // between the lookup above and this send)
                manager->complete_call(
                    session, false,
                    nlohmann::json::array({nlohmann::json::object(
                        {{"code", call_error_code_for(send_error)},
                         {"message", send_error},
                         {"retryable", false}})}));
                return session;
                // GCOVR_EXCL_STOP
            }
            return session;
        });

    shield.set_function("_is_in_exit",
                        [manager]() -> bool { return manager->is_in_exit(); });

    // Target-shape check for the call wrappers: returns the normalized
    // service id, or nil when the target is neither a ServiceHandle nor a
    // string. Shape errors are programming errors and are reported with
    // invalid_target regardless of the calling context.
    shield.set_function(
        "_call_target_id",
        [](sol::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           sol::object target) -> sol::optional<std::string> {
            const std::string id = extract_service_id(target);
            if (id.empty()) {
                return sol::nullopt;
            }
            return id;
        });

    // Stable error code for a raw call-failure message (used by the call
    // wrapper to shape non-table resume payloads into {code, message}).
    shield.set_function("_call_error_code",  // GCOVR_EXCL_LINE
                        [](sol::optional<    // GCOVR_EXCL_LINE
                            std::string>
                               msg) -> std::string {
                            return call_error_code_for(msg.value_or(""));
                        });

    // DEPRECATED: Use ctx.sender instead. Kept for backward compatibility.
    // In new code, prefer: function M.handler(ctx, ...) local src = ctx.sender
    // end
    shield.set_function("sender", [manager]() -> sol::optional<std::string> {
        // Returns nil in timer/fork context (no sender).
        // Returns nil outside any dispatch (module-level code).
        // The distinction between "no sender" and "context_expired" is
        // that in timer/fork context we ARE inside a dispatch scope but
        // the sender is empty; outside any scope the context is expired.
        const auto sender = manager->current_sender_id();
        if (sender.empty()) {
            return sol::nullopt;
        }
        return sender;
    });

    // DEPRECATED: Use ctx.trace instead. Kept for backward compatibility.
    shield.set_function("trace", [manager]() -> sol::optional<std::string> {
        const auto trace = manager->current_trace_id();
        if (trace.empty())  // GCOVR_EXCL_LINE (no active trace in coverage
            return sol::nullopt;  // suites reach this shim)
        return trace;             // GCOVR_EXCL_LINE (continuation)
    });

    // DEPRECATED: Use ctx.deadline instead. Kept for backward compatibility.
    shield.set_function("deadline", [manager]() -> sol::optional<int64_t> {
        const auto dl = manager->current_deadline_ms();
        if (dl <= 0)  // GCOVR_EXCL_LINE (no active deadline in coverage
            return sol::nullopt;  // suites reach this shim)
        return dl;                // GCOVR_EXCL_LINE (continuation)
    });

    // shield.call / shield.call_timeout are coroutine-only: inside a handler
    // coroutine (handler, timer/fork callback, on_init) the caller suspends
    // until the callee completes; on the main thread — module-level code —
    // they are rejected with a stable error code instead of blocking the
    // worker.
    sol::state_view lua(shield.lua_state());
    lua["shield"] = shield;
    lua.safe_script(
        "shield.call = function(target, method, ...)\n"
        "  if shield._is_in_exit() then\n"
        "    return false, {code='api_not_allowed_in_exit', "
        "message='shield.call is not allowed in on_exit'}\n"
        "  end\n"
        "  if shield._call_target_id(target) == nil then\n"
        "    return false, {code='invalid_target', "
        "message='target must be ServiceHandle or string', retryable=false}\n"
        "  end\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then\n"
        "    return false, {code='call_not_allowed_off_coroutine', "
        "message='shield.call is only allowed inside a coroutine'}\n"
        "  end\n"
        "  local session = shield._coro_call(target, method, table.pack(...), "
        "5000)\n"
        "  if session == 0 then\n"
        "    return false, {code='call_not_allowed_off_coroutine', "
        "message='shield.call is only allowed inside a coroutine'}\n"
        "  end\n"
        "  local r = table.pack(coroutine.yield())\n"
        "  if not r[1] then\n"
        "    if type(r[2]) == 'table' then return false, r[2] end\n"
        "    local msg = tostring(r[2])\n"
        "    return false, { code = shield._call_error_code(msg), message = "
        "msg "
        "}\n"
        "  end\n"
        "  return true, table.unpack(r, 2, r.n)\n"
        "end\n"
        "shield.call_timeout = function(timeout_ms, target, method, ...)\n"
        "  if shield._is_in_exit() then\n"
        "    return false, {code='api_not_allowed_in_exit', "
        "message='shield.call_timeout is not allowed in on_exit'}\n"
        "  end\n"
        "  if shield._call_target_id(target) == nil then\n"
        "    return false, {code='invalid_target', "
        "message='target must be ServiceHandle or string', retryable=false}\n"
        "  end\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then\n"
        "    return false, {code='call_not_allowed_off_coroutine', "
        "message='shield.call_timeout is only allowed inside a coroutine'}\n"
        "  end\n"
        "  local session = shield._coro_call(target, method, table.pack(...), "
        "timeout_ms)\n"
        "  if session == 0 then\n"
        "    return false, {code='call_not_allowed_off_coroutine', "
        "message='shield.call_timeout is only allowed inside a coroutine'}\n"
        "  end\n"
        "  local r = table.pack(coroutine.yield())\n"
        "  if not r[1] then\n"
        "    if type(r[2]) == 'table' then return false, r[2] end\n"
        "    local msg = tostring(r[2])\n"
        "    return false, { code = shield._call_error_code(msg), message = "
        "msg "
        "}\n"
        "  end\n"
        "  return true, table.unpack(r, 2, r.n)\n"
        "end",          // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::protected_function_result  // GCOVR_EXCL_LINE (gcov clone
                                           // artifact)
               pfr)                        // GCOVR_EXCL_LINE (gcov
                                           // clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
}

void register_timer_api(sol::table& shield, LuaServiceManager* manager,
                        LuaRuntime* runtime) {
    shield.set_function(
        "now", [manager]() -> int64_t { return manager->clock_now_ms(); });

    // Real monotonic milliseconds — NOT adjustable, for relative timing
    // (network backoff, heartbeat intervals) in Lua that must stay real.
    shield.set_function("monotonic", []() -> int64_t {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now)
            .count();
    });

    shield.set_function(
        "timer_once",
        [manager](int delay_ms,
                  sol::function callback) -> sol::variadic_results {
            sol::variadic_results results;
            sol::state_view lua(callback.lua_state());

            // Get current service ID
            const std::string service_id = manager->current_service_id();

            // Check timer limit.
            const auto timer_count = manager->active_actor_timer_count();
            if (timer_count >= kTimerLimit) {
                results.push_back(sol::make_object(lua, sol::nil));
                sol::this_state ts(callback.lua_state());
                results.push_back(
                    make_error(ts, "timer_limit", "timer limit reached"));
                return results;
            }

            const uint64_t id = service_id.empty()
                                    ? 0
                                    : manager->schedule_actor_timer_once(
                                          delay_ms, callback, service_id);
            results.push_back(sol::make_object(lua, id));
            return results;
        });

    shield.set_function(
        "timer",
        [manager](int interval_ms,
                  sol::function callback) -> sol::variadic_results {
            sol::variadic_results results;
            sol::state_view lua(callback.lua_state());

            // Get current service ID
            const std::string service_id = manager->current_service_id();

            // Check timer limit.
            const auto timer_count = manager->active_actor_timer_count();
            if (timer_count >= kTimerLimit) {
                results.push_back(sol::make_object(lua, sol::nil));
                sol::this_state ts(callback.lua_state());
                results.push_back(
                    make_error(ts, "timer_limit", "timer limit reached"));
                return results;
            }

            const uint64_t id = service_id.empty()
                                    ? 0
                                    : manager->schedule_actor_timer_fixed_delay(
                                          interval_ms, callback, service_id);
            results.push_back(sol::make_object(lua, id));
            return results;
        });

    shield.set_function(
        "cancel_timer",
        [manager](sol::this_state state, uint64_t id) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;

            const bool cancelled = manager->cancel_actor_timer(id);
            results.push_back(sol::make_object(lua, cancelled));
            if (!cancelled) {
                results.push_back(
                    make_error(state, "timer_not_found",
                               "Timer not found or already completed"));
            } else {
                results.push_back(sol::make_object(lua, sol::nil));
            }
            return results;
        });

    // shield.sleep is implemented as a Lua wrapper that schedules a native
    // timer to resume the current coroutine and then yields. The C primitive
    // _resume_after anchors the running coroutine against GC and arms the
    // timer; coroutine.yield suspends until the timer fires and resumes us.
    shield.set_function("_resume_after", [manager](sol::this_state state,
                                                   int delay_ms) {
        if (delay_ms < 0) {
            delay_ms = 0;
        }
        lua_State* co = state;  // current coroutine thread
        // Anchor the thread so it survives GC while suspended.
        lua_pushthread(co);
        const int ref = luaL_ref(co, LUA_REGISTRYINDEX);
        const std::string service_id = manager->current_service_id();
        auto resume_fn = [co, ref, manager, service_id]() {
            int nres = 0;
            // Driving-phase registration: a call completion arriving
            // while this guard is held is re-enqueued by resume_caller
            // instead of racing this resume.
            std::unique_ptr<LuaServiceManager::DrivingGuard> driving =
                std::make_unique<LuaServiceManager::DrivingGuard>(*manager, co);
            const int status = lua_resume(co, nullptr, 0, &nres);
            if (status == LUA_YIELD) {
                // Yielded again (e.g. another sleep/call): the API
                // that yielded has already anchored the coroutine for
                // its own resume source, so release this sleep anchor.
                luaL_unref(co, LUA_REGISTRYINDEX, ref);
                return;
            }
            // Terminal (LUA_OK or error): drop the live-coroutine entry.
            manager->note_coroutine_finished(co);
            // If this coroutine was servicing a call request that
            // yielded (e.g. the callee slept), route the response now
            // that it has completed. No-op for plain handlers.
            if (status == LUA_OK) {
                nlohmann::json returns = nlohmann::json::array();
                for (int i = 0; i < nres; ++i) {
                    sol::stack_object so(sol::state_view(co), i + 1);
                    returns.push_back(lua_to_json(so));
                }
                manager->on_handler_completed(co, returns);
            }
            // Terminal segment (LUA_OK or error): honor a shield.exit
            // the continuation requested. During spawn-init the spawn
            // path owns the request instead.
            if (!service_id.empty() &&
                !LuaServiceManager::spawn_init_in_progress()) {
                manager->finish_pending_exit(service_id);
            }
            // LUA_OK (completed) or an error: release the anchor.
            luaL_unref(co, LUA_REGISTRYINDEX, ref);
        };
        if (!service_id.empty()) {
            (void)manager->schedule_actor_timer_once_fn(
                delay_ms, std::move(resume_fn), service_id);
        }
    });

    // Blocking sleep used when shield.sleep is invoked outside any coroutine
    // (e.g. from module-level code). The coroutine-aware branch below handles
    // the yieldable case inside service handlers.
    shield.set_function("_block_sleep", [](int delay_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    });

    sol::state_view lua(shield.lua_state());
    lua["shield"] = shield;
    // Define shield.sleep in Lua so it can use coroutine.yield natively. When
    // not running inside a coroutine, block the calling thread so the call
    // still completes.
    lua.safe_script(
        "shield.sleep = function(ms)\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then\n"
        "    shield._block_sleep(ms)\n"
        "  else\n"
        "    shield._resume_after(ms); coroutine.yield()\n"
        "  end\n"
        "end",          // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::protected_function_result  // GCOVR_EXCL_LINE (gcov clone
                                           // artifact)
               pfr)                        // GCOVR_EXCL_LINE (gcov
                                           // clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
}

void register_task_api(sol::table& shield, LuaServiceManager* manager,
                       LuaRuntime* runtime) {
    (void)runtime;

    shield.set_function(
        "fork",
        [manager](sol::this_state state,
                  sol::function fn) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;
            const std::string service_id = manager->current_service_id();

            // Check fork limit.
            if (manager->pending_task_count(service_id) >= kForkLimit) {
                results.push_back(sol::make_object(lua, sol::nil));
                sol::this_state ts(fn.lua_state());
                results.push_back(
                    make_error(ts, "fork_limit", "fork limit reached"));
                return results;
            }
            // Capture the Lua function with its owning state_view. Execution
            // is dispatched to the owning service actor via fork_task_atom.
            // The task outlives the registering coroutine (on_init / any
            // handler runs as one), so re-anchor the function onto the main
            // thread's lua_State — otherwise the coroutine's lua_State may be
            // collected by the GC and every later use of the captured
            // reference would dangle (mirror of anchor_to_main_thread in
            // lua_service.cpp).
            lua_State* fn_state = fn.lua_state();
            lua_State* fn_main =
                fn_state == nullptr ? nullptr : sol::main_thread(fn_state);
            if (fn_main != nullptr && fn_main != fn_state &&
                fn.registry_index() != LUA_NOREF) {
                fn =
                    sol::function(fn_main, sol::ref_index(fn.registry_index()));
            }
            uint64_t task_id = manager->enqueue_forked_task(
                service_id,
                []() {
                    // GCOVR_EXCL_START (unreachable: shield.fork always hands
                    // a valid raw_fn, so the actor dispatches through the
                    // coroutine path and never runs this plain wrapper)
                    auto& log = shield::log::get_logger("lua");
                    SHIELD_LOG_ERROR(log, "task error: fork body missing");
                    // GCOVR_EXCL_STOP
                },    // GCOVR_EXCL_LINE (continuation)
                fn);  // raw_fn for coroutine wrapping
            results.push_back(sol::make_object(lua, task_id));
            return results;
        });
}

void register_config_api(sol::table& shield) {
    shield.set_function(
        "config",
        [](sol::this_state state, std::string key,
           sol::optional<sol::object> default_value) -> sol::object {
            sol::state_view lua(state);
            auto& config = shield::config::global_config();
            if (!config.has(key)) {
                if (default_value) {
                    return *default_value;
                }
                return sol::make_object(lua, sol::nil);
            }

            const auto value = config.get_string(key, "");
            if (value == "true") {
                return sol::make_object(lua, true);
            }
            if (value == "false") {
                return sol::make_object(lua, false);
            }

            // Require the whole string to be consumed so that "12abc"
            // doesn't get partially parsed as 12.  When the string
            // contains a decimal point or exponent marker, always parse
            // as double first to avoid truncation (e.g. "3.14" → 3).
            const bool looks_like_float =
                value.find('.') != std::string::npos ||
                value.find('e') != std::string::npos ||
                value.find('E') != std::string::npos;

            if (looks_like_float) {
                try {
                    size_t pos = 0;
                    const double parsed = std::stod(value, &pos);
                    if (pos == value.size()) {
                        return sol::make_object(lua, parsed);
                    }
                } catch (const std::exception&) {
                }
            } else {
                try {
                    size_t pos = 0;
                    const long long parsed = std::stoll(value, &pos);
                    if (pos == value.size()) {
                        return sol::make_object(lua, parsed);
                    }
                } catch (const std::exception&) {
                }

                try {
                    size_t pos = 0;
                    const double parsed = std::stod(value, &pos);
                    if (pos == value.size()) {
                        return sol::make_object(lua, parsed);
                    }
                } catch (const std::exception&) {
                }
            }

            return sol::make_object(lua, value);
        });
}

void register_log_api(sol::table& shield, LuaServiceManager* manager) {
    auto& log = shield::log::get_logger("lua");
    sol::state_view lua(shield.lua_state());
    auto log_table = lua.create_table();

    // Helper: build log message with service context prefix.
    auto build_msg = [manager](sol::object value) -> std::string {
        std::string msg = lua_to_json(value).dump();
        const std::string sid = manager->current_service_id();
        if (sid.empty()) return msg;
        return "[" + sid + "] " + msg;
    };

    log_table.set_function("debug", [&log, build_msg](sol::object value) {
        SHIELD_LOG_DEBUG(log, build_msg(value));
    });
    log_table.set_function("info", [&log, build_msg](sol::object value) {
        SHIELD_LOG_INFO(log, build_msg(value));
    });
    log_table.set_function("warn", [&log, build_msg](sol::object value) {
        SHIELD_LOG_WARNING(log, build_msg(value));
    });
    log_table.set_function("error", [&log, build_msg](sol::object value) {
        SHIELD_LOG_ERROR(log, build_msg(value));
    });

    shield["log"] = log_table;
}

void register_shield_api(LuaRuntime& runtime) { (void)runtime; }

namespace api {

using LuaRuntime = shield::lua::LuaRuntime;
using LuaServiceManager = shield::lua::LuaServiceManager;

void register_service_api(LuaRuntime& runtime) { (void)runtime; }

void register_message_api(LuaRuntime& runtime) { (void)runtime; }

void register_timer_api(LuaRuntime& runtime) { (void)runtime; }

void register_timer_api(sol::table& shield, LuaServiceManager* manager,
                        LuaRuntime* runtime) {
    (void)shield;
    (void)manager;
    (void)runtime;
}

void register_task_api(LuaRuntime& runtime) { (void)runtime; }

void register_config_api(LuaRuntime& runtime) { (void)runtime; }

void register_log_api(LuaRuntime& runtime) { (void)runtime; }

void register_gateway_api(LuaRuntime& runtime) { (void)runtime; }

}  // namespace api

// -- Client identity registration ---------------------------------------------
//
// ClientContext / ClientRef usertypes plus the __shield_make_client_context
// materializer used by json_to_lua and by the coroutine resume path. No
// constructor is exported: identity userdata is created by the runtime only.
void register_client_identity_api(sol::state& lua) {
    sol::usertype<ClientContextBox> context_type =
        lua.new_usertype<ClientContextBox>("ClientContext",
                                           sol::no_constructor);
    bind_identity_properties(context_type);
    context_type.set("ref",  // GCOVR_EXCL_LINE (gcov clone artifact)
                     [](const ClientContextBox& box,  // GCOVR_EXCL_LINE
                        sol::this_state s) {
                         return sol::make_object(s, ClientRefBox{box.data});
                     });
    sol::usertype<ClientRefBox> ref_type =
        lua.new_usertype<ClientRefBox>("ClientRef", sol::no_constructor);
    bind_identity_properties(ref_type);

#ifdef SHIELD_ENABLE_PLAYER
    // Read-only PlayerRef value userdata. It crosses services as the
    // __shield_player_ref marker JSON (see lua_to_json / json_to_lua).
    // Record-like access (ref.uid) is the documented Lua face, so the
    // accessors are registered as sol2 properties — a plain set() with a
    // unary lambda would expose them as methods (obj:uid()).
    sol::usertype<PlayerRefBox> player_ref_type =
        lua.new_usertype<PlayerRefBox>("PlayerRef", sol::no_constructor);
    player_ref_type.set("uid", sol::property([](const PlayerRefBox& box) {
                            return box.data.uid;
                        }));
    player_ref_type.set("node_id", sol::property([](const PlayerRefBox& box) {
                            return box.data.node_id;
                        }));
    player_ref_type.set("service_id",
                        sol::property([](const PlayerRefBox& box) {
                            return box.data.service_id;
                        }));
    player_ref_type.set("epoch", sol::property([](const PlayerRefBox& box) {
                            return box.data.epoch;
                        }));
#endif

    lua.set_function(
        "__shield_make_client_context",
        [](sol::this_state s, std::uint64_t session_id,
           std::uint32_t session_epoch, std::string player_id,
           std::string gateway_address, std::string protocol_profile_id) {
            return sol::make_object(
                s, ClientContextBox{ClientContextData{
                       std::move(gateway_address), session_id, session_epoch,
                       std::move(player_id), std::move(protocol_profile_id)}});
        });
}

// shield.client.* (bind / close) and the _client_* primitives behind them.
// The bind primitive suspends the caller coroutine exactly like _coro_call:
// the gateway actor completes the session through complete_call, and the
// Lua wrapper resumes with (true, client_ref) or (false, error_table).
void register_client_api(sol::table& shield, LuaServiceManager* manager) {
    shield.set_function(
        "_client_bind",
        [manager](sol::this_state state, sol::object client,
                  std::string player_id, std::string target_service,
                  int timeout_ms) -> uint64_t {
            ClientContextData data;
            if (!client_arg_to_data(client, &data) || player_id.empty() ||
                target_service.empty()) {
                return 0;
            }
            // Suspend first: every failure below completes the session
            // asynchronously so the wrapper's coroutine.yield() always gets
            // exactly one resume.
            const uint64_t session =
                manager->suspend_for_call(state, timeout_ms);
            caf::actor gateway = manager->gateway_actor(data.gateway_address);
            if (gateway == nullptr) {
                manager->complete_call(
                    session, false,
                    nlohmann::json::array({nlohmann::json::object(
                        {{"code", "client_rpc.epoch_expired"},
                         {"message",
                          "gateway not found: " + data.gateway_address}})}));
                return session;
            }
            ClientBindRequest request;
            request.call_session = session;
            request.sender_service = manager->current_service_id();
            request.context = data;
            request.player_id = std::move(player_id);
            request.target_service = std::move(target_service);
            caf::anon_send(gateway, std::move(request));
            return session;
        });

    shield.set_function(
        "_client_close",
        [manager](sol::object client, std::string reason) -> bool {
            ClientContextData data;
            if (!client_arg_to_data(client, &data)) {
                return false;
            }
            caf::actor gateway = manager->gateway_actor(data.gateway_address);
            if (gateway == nullptr) {
                return false;
            }
            ClientCloseRequest request;
            request.context = data;
            request.reason = std::move(reason);
            caf::anon_send(gateway, std::move(request));
            return true;
        });

    shield.set_function(
        "_client_egress",
        [manager](sol::object client, uint32_t route_id,
                  sol::object payload) -> bool {
            ClientContextData data;
            if (!client_arg_to_data(client, &data)) {
                return false;
            }
            caf::actor gateway = manager->gateway_actor(data.gateway_address);
            if (gateway == nullptr) {
                return false;
            }
            ClientEgress egress;
            egress.context = data;
            egress.route_id = route_id;
            if (payload.is<sol::table>()) {
                egress.message = lua_to_json(payload);
            } else if (payload.is<std::string>()) {
                const auto value = payload.as<std::string>();
                egress.body_bytes.assign(value.begin(), value.end());
            } else {
                return false;
            }
            caf::anon_send(gateway, std::move(egress));
            return true;
        });

    sol::state_view lua(shield.lua_state());
    sol::table client = lua.create_table();
    shield["client"] = client;
    // The wrapper bodies resolve the shield table as a global at CALL time
    // (register_full_shield_api assigns lua["shield"] afterwards), so the
    // chunk only returns the functions instead of touching the global.
    sol::function bind_fn = lua.safe_script(
        "return function(client, player_id, target)\n"
        "  if shield._is_in_exit() then\n"
        "    return false, {code='api_not_allowed_in_exit', "
        "message='shield.client.bind is not allowed in on_exit'}\n"
        "  end\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then\n"
        "    return false, {code='call_not_allowed_off_coroutine', "
        "message='shield.client.bind requires a handler coroutine'}\n"
        "  end\n"
        "  local session = shield._client_bind(client, player_id, target, "
        "5000)\n"
        "  if session == 0 then\n"
        "    return false, {code='invalid_client_reference', "
        "message='bind requires a ClientContext or ClientRef and a "
        "non-empty player_id and target'}\n"
        "  end\n"
        "  local r = table.pack(coroutine.yield())\n"
        "  if not r[1] then return false, r[2] end\n"
        "  return true, r[2]\n"
        "end\n",        // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::protected_function_result  // GCOVR_EXCL_LINE (gcov clone
                                           // artifact)
               pfr)                        // GCOVR_EXCL_LINE (gcov
                                           // clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
    client["bind"] = bind_fn;
    sol::function close_fn = lua.safe_script(
        "return function(client, reason)\n"
        "  return shield._client_close(client, reason or 'kicked')\n"
        "end\n",        // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::protected_function_result  // GCOVR_EXCL_LINE (gcov clone
                                           // artifact)
               pfr)                        // GCOVR_EXCL_LINE (gcov
                                           // clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
    client["close"] = close_fn;
}

// Registers one shield.client_rpc.<name> helper bound to a server-to-client
// descriptor route. Called per service VM at spawn time (after the
// descriptor table is compiled).
void register_client_rpc_helper(sol::state& lua, LuaServiceManager* manager,
                                std::string_view name, uint32_t route_id) {
    sol::table shield = lua["shield"];
    sol::table client_rpc = shield["client_rpc"];
    // Reverse map for the player client_message guard (route_id -> name);
    // absent names degrade to "route_<id>" at the guard call site.
    {
        sol::object names_obj = shield["_client_route_names"];
        if (!names_obj.valid() || !names_obj.is<sol::table>()) {
            names_obj = lua.create_table();
            shield["_client_route_names"] = names_obj;
        }
        names_obj.as<sol::table>()[route_id] = std::string(name);
    }
    client_rpc.set_function(
        std::string(name),
        [manager, route_id](sol::object client, sol::object payload) -> bool {
            ClientContextData data;
            if (!client_arg_to_data(client, &data)) {
                return false;
            }
            caf::actor gateway = manager->gateway_actor(data.gateway_address);
            if (gateway == nullptr) {
                return false;
            }
            ClientEgress egress;
            egress.context = data;
            egress.route_id = route_id;
            if (payload.is<sol::table>()) {
                egress.message = lua_to_json(payload);
            } else if (payload.is<std::string>()) {
                const auto value = payload.as<std::string>();
                egress.body_bytes.assign(value.begin(), value.end());
            } else {
                return false;
            }
            caf::anon_send(gateway, std::move(egress));
            return true;
        });
}

#ifdef SHIELD_ENABLE_CLUSTER
void register_cluster_api(sol::table& shield, LuaServiceManager* manager) {
    sol::state_view lua(shield.lua_state());
    auto cluster = lua.create_table();

    // shield.cluster.query(node_id, service_name) -> service_id or nil, error
    cluster.set_function(  // GCOVR_EXCL_LINE (gcov continuation artifact)
        "query",
        [](sol::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string node_id,    // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string service_name) -> sol::variadic_results {
            sol::state_view lua(state);
            sol::variadic_results results;

            auto* cluster_manager = shield::cluster::global_cluster_manager();
            if (!cluster_manager) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(make_error(state, "module_unavailable",
                                             "shield_cluster is not enabled"));
                return results;
            }

            const auto reachable =
                cluster_manager->check_node_reachable(node_id);
            if (!reachable.empty()) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(
                    make_error(state, reachable,
                               "cluster node is not reachable: " + node_id));
                return results;
            }

            auto service_id =
                cluster_manager->query_remote(node_id, service_name);
            if (service_id.empty()) {
                results.push_back(sol::make_object(lua, sol::nil));
                results.push_back(
                    make_error(state, "service_not_found",
                               "remote service not found: " + service_name));
                return results;
            }

            results.push_back(sol::make_object(lua, service_id));
            results.push_back(sol::make_object(lua, sol::nil));
            return results;
        });

    // shield.cluster.nodes() -> table of node info
    cluster.set_function("nodes", [](sol::this_state state) -> sol::table {
        sol::state_view lua(state);
        sol::table nodes = lua.create_table();
        auto* cluster_manager = shield::cluster::global_cluster_manager();
        if (!cluster_manager) {
            return nodes;
        }
        int index = 1;
        for (const auto& node : cluster_manager->nodes()) {
            sol::table entry = lua.create_table();
            entry["node_id"] = node.node_id;
            entry["address"] = node.address;
            entry["state"] = shield::cluster::node_state_name(node.state);
            entry["last_heartbeat_ms"] = node.last_heartbeat_ms;
            entry["connected_at_ms"] = node.connected_at_ms;
            // Decimal string: see the node_epoch() binding above.
            entry["epoch"] = std::to_string(node.epoch);
            nodes[index++] = entry;
        }
        return nodes;
    });

    // shield.cluster.node_id() -> this node's ID
    cluster.set_function(  // GCOVR_EXCL_LINE (gcov continuation artifact)
        "node_id",
        [](sol::this_state state)  // GCOVR_EXCL_LINE (lambda entry artifact)
        -> sol::optional<std::string> {  // GCOVR_EXCL_LINE (lambda entry
                                         // artifact)
            auto* cluster_manager = shield::cluster::global_cluster_manager();
            if (!cluster_manager || cluster_manager->node_id().empty()) {
                return sol::nullopt;
            }
            return cluster_manager->node_id();
        });

    cluster.set_function("node_epoch", []() -> sol::optional<std::string> {
        auto* cluster_manager = shield::cluster::global_cluster_manager();
        if (!cluster_manager) {
            return sol::nullopt;
        }
        // Serialized as a decimal string: uint64 does not survive the
        // Lua number (double, 53-bit mantissa) round-trip.
        return std::to_string(cluster_manager->node_epoch());
    });

    shield["cluster"] = cluster;
}
#endif

#ifdef SHIELD_ENABLE_PLAYER
namespace {

// sol table_proxy has no get_or_default; read fields defensively instead so
// a malformed ref table degrades to an empty field (mirroring the
// client-identity from_json behavior).
std::string player_ref_string(const sol::table& t, const char* key) {
    sol::object v = t[key];
    return v.is<std::string>() ? v.as<std::string>() : std::string();
}

std::uint64_t player_ref_epoch(const sol::table& t) {
    sol::object v = t["epoch"];
    // Epoch arrives as a decimal string (the Lua double round-trip would
    // truncate a full uint64) but accept a number for convenience.
    if (v.is<std::string>()) {
        try {
            return std::stoull(v.as<std::string>());
        } catch (const std::exception&) {
            return 0;
        }
    }
    if (v.is<std::uint64_t>()) return v.as<std::uint64_t>();
    if (v.is<int>()) return static_cast<std::uint64_t>(v.as<int>());
    return 0;
}

shield::player::PlayerRef player_ref_from_table(const sol::table& t) {
    shield::player::PlayerRef ref;
    ref.uid = player_ref_string(t, "uid");
    ref.node_id = player_ref_string(t, "node_id");
    ref.service_id = player_ref_string(t, "service_id");
    ref.epoch = player_ref_epoch(t);
    return ref;
}  // GCOVR_EXCL_LINE (function-exit arc artifact of player_ref_from_table)

// Shared snapshot shape for get/resolve: flat read-only fields plus a
// materialized PlayerRef under `ref`.
sol::table write_session(sol::state_view s,
                         const shield::player::SessionInfo& info) {
    sol::table out = s.create_table();
    out["uid"] = info.ref.uid;
    out["node_id"] = info.ref.node_id;
    out["service_id"] = info.ref.service_id;
    out["epoch"] = std::to_string(info.ref.epoch);
    out["state"] = shield::player::session_state_name(info.state);
    out["device_id"] = info.device_id;
    out["ref"] = sol::make_object(
        s, PlayerRefBox{{info.ref.uid, info.ref.node_id, info.ref.service_id,
                         info.ref.epoch}});
    return out;
}  // GCOVR_EXCL_LINE (function-exit arc artifact of write_session)

}  // namespace

// -----------------------------------------------------------------------------
// shield_player (P0): session admission (C++ PlayerManager) + the Lua-first
// session state machine (this orchestration chunk). See runtime-player.md,
// "shield_player 模块契约(P0)".
//
// The chunk runs once per VM. All shield.* references inside are resolved at
// CALL time: register_player_api runs before lua["shield"] is assigned.
// -----------------------------------------------------------------------------
constexpr const char* kPlayerOrchestration = R"lua(
local impl = {
    installed = false,
    M = nil,
    hooks = nil,
    instance_script = nil,
    session = nil,
    -- pending_* carry what the spawning side passed through spawn args and
    -- are consumed by on_bound on the player-instance VM.
    pending_auth_result = nil,
    pending_device_id = nil,
    data = {},
    stats = {rejected_not_ready = 0, rejected_by_guard = 0, offline_dropped = 0},
}

local REQUIRED = {'auth', 'login', 'client_message', 'disconnect', 'logout'}
local OPTIONAL = {'ready', 'reconnect', 'save'}

local function err(code, message)
    return {code = code, message = message or code, retryable = false}
end

local function now_ms()
    return shield.player.now_ms()
end

local function node_info()
    return shield.player.node_info()
end

local function mgr()
    return shield.player.manager
end

-- Fresh PlayerRef for a locally hosted uid (runtime-player.md: node_id and
-- epoch come from node_info(), service name follows one-player-one-service).
local function make_ref(uid)
    local n = node_info()
    return {uid = uid, node_id = n.node_id,
            service_id = 'player_' .. uid, epoch = n.epoch}
end

-- Per-VM default hook implementations (runtime-player.md 钩子表). Each one
-- is a real behavior, never a silent noop: business hooks may delegate back
-- to them explicitly via shield.player.defaults.*.
local defaults = {}

function defaults.allow(ctx, client, route_name, request)
    return true
end

function defaults.ready(ctx, client)
    if impl.session then
        impl.session.state_name = 'ready'
        mgr().set_state(impl.session.uid, 'ready')
    end
    return true
end

function defaults.reconnect(ctx, client)
    if not impl.session then return false end
    -- The restored session's live ClientRef is the fresh one the spawner
    -- bound; queued offline messages must egress through it, not the stale
    -- pre-disconnect ref.
    if client then impl.session.client_ref = client end
    impl.session.state_name = 'ready'
    mgr().set_state(impl.session.uid, 'ready')
    impl.flush_offline()
    return true
end

function defaults.save(ctx, reason)
    -- persistence (OD-009): serialize whitelist fields into the instance's
    -- player_save(uid, fields) when the business provides one; with no
    -- persistence configured this is an honest no-op.
    if not impl.session then return true end
    local cfg = shield.player.config()
    local allow = {}
    for _, k in ipairs(cfg.persistence_fields or {}) do allow[k] = true end
    local fields = {}
    for k, v in pairs(impl.data) do
        if allow[k] then fields[k] = v end
    end
    local M = impl.M
    if type(M.player_save) == 'function' then
        local ok, e = pcall(M.player_save, impl.session.uid, fields)
        if not ok then
            if cfg.persistence_panic then
                error('persistence_save_failed: ' .. tostring(e))
            end
            shield.log.warn('persistence_save_failed: ' .. tostring(e))
        end
    end
    return true
end

function impl.push_online(client_ref, route_name, payload)
    local helper = shield.client_rpc[route_name]
    if type(helper) ~= 'function' then return false end
    return helper(client_ref, payload) == true
end

function impl.flush_offline()
    local s = impl.session
    if not s then return end
    local q = s.offline
    s.offline = {}
    for i = 1, #q do
        local item = q[i]
        if not impl.push_online(s.client_ref, item.route, item.payload) then
            impl.stats.offline_dropped = impl.stats.offline_dropped + 1
        end
    end
end

function impl.save_now(reason)
    local ok, e = pcall(impl.hooks.save, nil, reason)
    if not ok then
        shield.log.warn('player save hook failed: ' .. tostring(e))
    end
end

function impl.do_logout(ctx, client, reason)
    local s = impl.session
    if not s then return end
    impl.session = nil
    mgr().unregister(s.uid)
    local ok, e = pcall(impl.hooks.logout, ctx, client, reason)
    if not ok then
        shield.log.warn('player logout hook failed: ' .. tostring(e))
    end
    impl.save_now('logout')
    s.offline = {}
end

-- Bound control message on a player-instance VM: first bind of the session.
function impl.on_bound(ctx, client)
    if not impl.installed then return end
    if impl.session then return end
    -- ClientContext userdata exposes identity through method accessors
    -- (runtime-player.md: client:player_id()); a marker table would carry
    -- them as fields. Handles both.
    local function client_field(c, name)
        if type(c) == 'userdata' then
            local ok, v = pcall(c[name], c)
            if ok then return v end
            return nil
        elseif type(c) == 'table' then
            return c[name]
        end
        return nil
    end
    local uid = client_field(client, 'player_id')
    if not uid or uid == '' then return end
    local s = {
        uid = uid,
        service_name = 'player_' .. uid,
        state_name = 'online',
        offline = {},
        client_ref = client_field(client, 'ref'),
        device_id = impl.pending_device_id or '',
    }
    impl.session = s
    local n = node_info()
    mgr().register_session(
        {uid = uid, node_id = n.node_id, service_id = s.service_name,
         epoch = n.epoch},
        s.device_id, 'online', now_ms())
    local auth_result = impl.pending_auth_result or {}
    impl.pending_auth_result = nil
    impl.pending_device_id = nil
    impl.hooks.login(ctx, client, auth_result)
    impl.hooks.ready(ctx, client)
end

function impl.on_disconnected(ctx, client, reason)
    if not impl.installed or not impl.session then return end
    local uid = impl.session.uid
    impl.session.state_name = 'disconnected'
    mgr().mark_disconnected(uid, now_ms())
    local ok, e = pcall(impl.hooks.disconnect, ctx, client, reason)
    if not ok then
        shield.log.warn('player disconnect hook failed: ' .. tostring(e))
    end
    local cfg = shield.player.config()
    local window = cfg.reconnect_window_ms or 30000
    if window > 0 then
        shield.timer_once(window, function()
            if impl.session and impl.session.uid == uid and
                impl.session.state_name == 'disconnected' then
                impl.do_logout(nil, nil, 'timeout')
            end
        end)
    else
        impl.do_logout(nil, nil, 'timeout')
    end
end

function impl.on_unbound(ctx, client, reason)
    if not impl.installed or not impl.session then return end
    impl.do_logout(ctx, client, reason or 'unbound')
end

-- player:authenticate(ctx, client, request) — called inside a pre-login
-- route handler coroutine on the auth-entry service (runtime-player.md
-- 认证与状态机, 5 steps).
function impl.authenticate(ctx, client, request)
    if not impl.installed then
        return false, err('module_unavailable', 'player not set up')
    end
    -- 1. auth hook: table result, or true+player_id, or false/nil+code.
    local r = table.pack(impl.hooks.auth(ctx, client, request))
    if r[1] == false or r[1] == nil then
        local code = type(r[2]) == 'string' and r[2] or 'auth_failed'
        return false, err(code, 'auth rejected: ' .. code)
    end
    local auth_result = r[1]
    if auth_result == true then auth_result = {player_id = r[2]} end
    if type(auth_result) ~= 'table' or type(auth_result.player_id) ~= 'string'
        or auth_result.player_id == '' then
        return false, err('auth_failed',
                          'auth hook must yield a non-empty player_id')
    end
    -- 2. anonymous/spectator opt-in gate.
    local cfg = shield.player.config()
    if auth_result.anonymous and not cfg.anonymous then
        return false, err('anonymous_disabled',
                          'anonymous logins are disabled')
    end
    if auth_result.spectator and not cfg.spectator then
        return false, err('spectator_disabled',
                          'spectator logins are disabled')
    end
    -- 3. PlayerManager admission ruling. The device fingerprint travels on
    -- the login request unless the auth hook attached one to its result.
    local uid = auth_result.player_id
    local device_id = auth_result.device_id
        or (type(request) == 'table' and request.device_id) or ''
    local now = now_ms()
    local d = mgr().admit(uid, device_id, now)
    if not d then
        return false, err('module_unavailable', 'player manager unavailable')
    end
    if d.kind == 'reject' then
        return false, err(d.code, 'admission rejected')
    end
    if d.kind == 'kick_old' and d.kicked_service_id and
        d.kicked_service_id ~= '' then
        shield.send(d.kicked_service_id, 'player_logout',
                    {reason = 'replaced'})
    end
    local ref = make_ref(uid)
    if d.kind == 'restore' then
        -- Reconnect-window restore: reuse the live instance, no respawn.
        local ok, second = shield.client.bind(client, uid, ref.service_id)
        if not ok then return false, second end
        mgr().mark_reconnected(uid, now)
        -- The instance must flush through the fresh-epoch ref bind just
        -- produced: the caller's client still carries the pre-reconnect
        -- epoch and the gateway drops its egress as stale.
        shield.send(ref.service_id, 'player_reconnect', {client = second})
        return true, second
    end
    -- 4. fresh spawn (one-player-one-service) + bind.
    if type(impl.instance_script) ~= 'string' or
        impl.instance_script == '' then
        return false, err('instance_script_required',
                          'setup opts.instance_script is required to spawn '
                              .. 'player instances')
    end
    local spawn_opts = {name = ref.service_id,
                        args = {player_id = uid, device_id = device_id,
                                auth_result = auth_result}}
    if impl.instance_routes then
        spawn_opts.rpc = {routes = impl.instance_routes}
    end
    local _, spawn_err = shield.spawn(impl.instance_script, spawn_opts)
    if spawn_err then return false, spawn_err end
    local ok, second = shield.client.bind(client, uid, ref.service_id)
    if not ok then return false, second end
    -- 5. success: the fresh-epoch ClientRef from bind.
    return true, second
end

function impl.push(target, route_name, payload)
    if not impl.installed then
        return false, err('module_unavailable', 'player not set up')
    end
    local uid
    if type(target) == 'string' then
        uid = target
    elseif type(target) == 'userdata' or type(target) == 'table' then
        uid = target.uid
    end
    if not uid or uid == '' then
        return false, err('invalid_player_ref',
                          'push target must be a uid or PlayerRef')
    end
    if impl.session and impl.session.uid == uid then
        if impl.session.state_name == 'ready' or
            impl.session.state_name == 'online' then
            if impl.push_online(impl.session.client_ref, route_name,
                                payload) then
                return true
            end
            return false, err('push_route_not_found',
                              'no s2c helper for route: ' ..
                                  tostring(route_name))
        end
        -- Offline (disconnected window): queue for the reconnect flush.
        local cfg = shield.player.config()
        local limit = cfg.message_queue_limit or 64
        if #impl.session.offline >= limit then
            impl.stats.offline_dropped = impl.stats.offline_dropped + 1
            return false, err('offline_queue_full',
                              'message queue limit reached')
        end
        table.insert(impl.session.offline,
                     {route = route_name, payload = payload})
        return true
    end
    return false, err('player_not_found',
                      'player is not hosted by this service')
end

function impl.setup(M, opts)
    if type(M) ~= 'table' then
        return nil, err('setup_invalid', 'module table required')
    end
    if impl.installed then
        return nil, err('setup_invalid', 'player module already set up')
    end
    opts = opts or {}
    local hooks = {}
    for _, name in ipairs(REQUIRED) do
        local v = opts[name]
        if type(v) == 'function' then
            hooks[name] = v
        elseif type(v) == 'string' and type(M[v]) == 'function' then
            hooks[name] = M[v]
        else
            return nil, err('setup_invalid',
                            'missing required hook: ' .. name)
        end
    end
    for _, name in ipairs(OPTIONAL) do
        local v = opts[name]
        if type(v) == 'function' then
            hooks[name] = v
        elseif type(v) == 'string' and type(M[v]) == 'function' then
            hooks[name] = M[v]
        else
            hooks[name] = defaults[name]
        end
    end
    impl.hooks = hooks
    impl.M = M
    impl.instance_script = opts.instance_script
    impl.instance_routes = opts.instance_routes
    impl.installed = true

    -- Session lifecycle: wrap the gateway control-message methods so the
    -- gateway-layer module hooks (business on_client_bound etc.) and the
    -- player hooks keep working side by side (runtime-player.md).
    local prev_bound = M.on_client_bound
    M.on_client_bound = function(ctx, client)
        if prev_bound then prev_bound(ctx, client) end
        impl.on_bound(ctx, client)
    end
    local prev_disc = M.on_disconnect
    M.on_disconnect = function(ctx, client, reason)
        if prev_disc then prev_disc(ctx, client, reason) end
        impl.on_disconnected(ctx, client, reason)
    end
    local prev_unbound = M.on_client_unbound
    M.on_client_unbound = function(ctx, client, reason)
        if prev_unbound then prev_unbound(ctx, client, reason) end
        impl.on_unbound(ctx, client, reason)
    end
    -- spawn args reach the instance through on_init (bootstrap-like).
    local prev_init = M.on_init
    M.on_init = function(args)
        if prev_init then prev_init(args) end
        args = args or {}
        impl.pending_auth_result = args.auth_result
        impl.pending_device_id = args.device_id
    end
    -- Framework-driven instance methods (business may not override these
    -- names; they carry the replaced/timeout logout and reconnect paths).
    -- Both arrive through shield.send, whose dispatch always prepends the
    -- ctx table before the caller's arg table.
    M.player_logout = function(ctx, args)
        local reason = type(args) == 'table' and args.reason or args
        impl.do_logout(ctx, nil, reason or 'logout')
    end
    M.player_reconnect = function(ctx, args)
        if impl.installed and impl.session then
            local client = type(args) == 'table' and args.client or nil
            impl.hooks.reconnect(ctx, client)
        end
    end

    -- Periodic save (0 = logout-only).
    local cfg = shield.player.config()
    if (cfg.save_interval_ms or 0) > 0 then
        shield.timer(cfg.save_interval_ms, function()
            if impl.session then impl.save_now('interval') end
        end)
    end

    -- The client-message guard for invoke_client_rpc (see lua_runtime.cpp).
    -- Sessions that do not exist on this VM (the auth-entry service, or any
    -- message before the first bind) pass through untouched. Ready-state
    -- truth lives in the PlayerManager so every observer agrees on it.
    rawset(_G, '__shield_player_guard', function(ctx, client, route_name,
                                                 request)
        if not impl.session then return true end
        local state_name = impl.session.state_name
        local sess = mgr().get(impl.session.uid)
        if sess then state_name = sess.state end
        if state_name ~= 'ready' then
            impl.stats.rejected_not_ready = impl.stats.rejected_not_ready + 1
            return false, 'not_ready'
        end
        local ok, code = impl.hooks.client_message(ctx, client, route_name,
                                                   request)
        if ok then return true end
        impl.stats.rejected_by_guard = impl.stats.rejected_by_guard + 1
        return false, code or 'rejected'
    end)

    -- The per-module facade.
    local f = {}
    function f:authenticate(ctx, client, request)
        return impl.authenticate(ctx, client, request)
    end
    function f:push(target, route_name, payload)
        return impl.push(target, route_name, payload)
    end
    function f:set_data(key, value) impl.data[key] = value end
    function f:get_data(key) return impl.data[key] end
    function f:session()
        if not impl.session then return nil end
        local s = impl.session
        return {uid = s.uid, state = s.state_name, device_id = s.device_id,
                service_id = s.service_name}
    end
    function f:logout(reason)
        return impl.do_logout(nil, nil, reason or 'logout')
    end
    return f
end

-- shield.player.Base (P2 syntax sugar, OD-014): collects the hooks from the
-- module table by their setup field names and delegates to impl.setup — no
-- second lifecycle, no inheritance, no on_* prefix. opts carries only the
-- non-hook options (instance_script/instance_routes); an explicit hook
-- function in opts wins over the same-named module method.
local ALL_HOOKS = {}
for _, name in ipairs(REQUIRED) do table.insert(ALL_HOOKS, name) end
for _, name in ipairs(OPTIONAL) do table.insert(ALL_HOOKS, name) end

local Base = {}
function Base.setup(M, opts)
    local merged = {}
    for _, name in ipairs(ALL_HOOKS) do
        if type(M[name]) == 'function' then merged[name] = M[name] end
    end
    if type(opts) == 'table' then
        for _, key in ipairs({'instance_script', 'instance_routes'}) do
            merged[key] = opts[key]
        end
        for _, name in ipairs(ALL_HOOKS) do
            if type(opts[name]) == 'function' then merged[name] = opts[name] end
        end
    end
    return impl.setup(M, merged)
end

return {setup = impl.setup, defaults = defaults, impl = impl, Base = Base}
)lua";

void register_player_api(sol::table& shield, LuaServiceManager* manager) {
    sol::state_view lua(shield.lua_state());

    // Run the orchestration chunk once per VM; it returns the impl table.
    sol::object impl_obj = lua.safe_script(
        kPlayerOrchestration,
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::protected_function_result
               pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
    sol::table impl = impl_obj;

    auto player = lua.create_table();

    player.set_function(
        "setup",
        [impl](sol::this_state state, sol::table M,
               sol::object opts) -> sol::variadic_results {
            sol::state_view s(state);
            sol::variadic_results results;
            sol::protected_function setup = impl["setup"];
            sol::protected_function_result r =
                opts.valid() ? setup(M, opts) : setup(M);
            if (!r.valid()) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(make_error(state, "setup_invalid",
                                             "player setup raised an error"));
                return results;
            }
            for (unsigned int i = 0; i < r.return_count(); ++i) {
                results.push_back(r.get<sol::object>(i));
            }
            return results;
        });

    player.set_function("config", [](sol::this_state state) -> sol::table {
        sol::state_view s(state);
        auto* pm = shield::player::PlayerManager::global();
        sol::table cfg = s.create_table();
        if (pm == nullptr) return cfg;
        const auto& c = pm->config();
        cfg["multi_device"] =  // GCOVR_EXCL_LINE (gcov attributes no code to
                               // this line)
            c.multi_device == shield::player::MultiDevicePolicy::kSingle
                ? "single"
                : (c.multi_device == shield::player::MultiDevicePolicy::kKickOld
                       ? "kick_old"
                       : "multi");
        cfg["max_devices"] = c.max_devices;
        cfg["anonymous"] = c.anonymous_enabled;
        cfg["spectator"] = c.spectator_enabled;
        cfg["reconnect_window_ms"] = c.reconnect_window_ms;
        cfg["message_queue_limit"] = c.message_queue_limit;
        sol::table persistence = s.create_table();
        persistence["binding"] = c.persistence_binding;
        persistence["panic"] = c.persistence_panic_on_error;
        persistence["save_interval_ms"] = c.save_interval_ms;
        sol::table fields = s.create_table();
        int i = 1;
        for (const auto& f : c.persistence_fields) {
            fields[i++] = f;
        }
        persistence["fields"] = fields;
        cfg["persistence"] = persistence;
        return cfg;
    });

    // Wall-clock milliseconds for the manager's window arithmetic.
    player.set_function("now_ms", []() -> std::uint64_t {
        const auto now = std::chrono::system_clock::now();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch())
                .count());
    });

    // Locality for PlayerRef values. epoch travels as a decimal string:
    // uint64 does not survive the Lua double round-trip (see the cluster
    // node_epoch() binding for the same decision).
    player.set_function("node_info", [](sol::this_state state) -> sol::table {
        sol::state_view s(state);
        auto* pm = shield::player::PlayerManager::global();
        sol::table info = s.create_table();
        info["node_id"] = pm ? pm->node_id() : std::string();
        info["epoch"] = std::to_string(pm ? pm->node_epoch() : 0);
        return info;
    });

    player.set_function("stats", [impl](sol::this_state state) -> sol::table {
        sol::state_view s(state);
        // The chunk returns the wrapper {setup, defaults, impl}; the
        // orchestration state (and its stats counters) lives one level
        // deeper, next to what the Lua-side closures mutate.
        sol::object wrapper = impl.raw_get<sol::object>("impl");
        sol::object impl_stats =
            wrapper.is<sol::table>()
                ? wrapper.as<sol::table>().raw_get<sol::object>("stats")
                : sol::nil;
        sol::table out = s.create_table();
        if (impl_stats.is<sol::table>()) {
            sol::table stats_tbl = impl_stats;
            // Read each counter explicitly: a proxy-to-proxy assignment
            // (out["k"] = stats_tbl["k"]) pushes nothing and silently
            // produces an empty table.
            out.raw_set("rejected_not_ready",
                        stats_tbl.get<sol::object>("rejected_not_ready"));
            out.raw_set("rejected_by_guard",
                        stats_tbl.get<sol::object>("rejected_by_guard"));
            out.raw_set("offline_dropped",
                        stats_tbl.get<sol::object>("offline_dropped"));
        }
        return out;
    });

    // ---- manager: the uid index operations ----
    sol::table manager_tbl = lua.create_table();

    manager_tbl.set_function(
        "admit",
        [](sol::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string uid,        // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string device_id,  // GCOVR_EXCL_LINE (gcov attributes no code
                                   // to this line)
           std::uint64_t now_ms) -> sol::variadic_results {
            sol::state_view s(state);
            sol::variadic_results results;
            auto* pm = shield::player::PlayerManager::global();
            if (pm == nullptr) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(make_error(state, "module_unavailable",
                                             "player manager unavailable"));
                return results;
            }
            auto d = pm->admit(uid, device_id, now_ms);
            sol::table out = s.create_table();
            switch (d.kind) {
                case shield::player::AdmissionDecision::Kind::kAllow:
                    out["kind"] = "allow";
                    break;
                case shield::player::AdmissionDecision::Kind::kRestore:
                    out["kind"] = "restore";
                    break;
                case shield::player::AdmissionDecision::Kind::kKickOld:
                    out["kind"] = "kick_old";
                    break;
                case shield::player::AdmissionDecision::Kind::kReject:
                    out["kind"] = "reject";
                    break;
            }
            out["code"] = d.code;
            out["kicked_service_id"] = d.kicked_service_id;
            results.push_back(sol::make_object(s, out));
            results.push_back(sol::make_object(s, sol::nil));
            return results;
        });

    manager_tbl.set_function(
        "register_session",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](sol::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           sol::table ref,         // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string device_id,  // GCOVR_EXCL_LINE (gcov attributes no code
                                   // to this line)
           std::string state_name, std::uint64_t now_ms) -> sol::object {
            auto* pm = shield::player::PlayerManager::global();
            if (pm == nullptr) return sol::make_object(state, sol::nil);
            const shield::player::PlayerRef player_ref =
                player_ref_from_table(ref);
            auto state_of = [](const std::string& name) {
                if (name == "online")
                    return shield::player::SessionState::kOnline;
                if (name == "ready")
                    return shield::player::SessionState::kReady;
                if (name == "anonymous")
                    return shield::player::SessionState::kAnonymous;
                if (name == "spectator")
                    return shield::player::SessionState::kSpectator;
                return shield::player::SessionState::kConnecting;
            };
            pm->register_session(player_ref, device_id, state_of(state_name),
                                 now_ms);
            return sol::make_object(state, true);
        });

    manager_tbl.set_function(
        "mark_disconnected",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](sol::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string
               uid,  // GCOVR_EXCL_LINE (gcov attributes no code to this line)
           std::uint64_t now_ms) -> bool {
            auto* pm = shield::player::PlayerManager::global();
            return pm != nullptr && pm->mark_disconnected(uid, now_ms);
        });

    manager_tbl.set_function(
        "mark_reconnected",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](sol::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string
               uid,  // GCOVR_EXCL_LINE (gcov attributes no code to this line)
           std::uint64_t now_ms) -> bool {
            auto* pm = shield::player::PlayerManager::global();
            return pm != nullptr && pm->mark_reconnected(uid, now_ms);
        });

    manager_tbl.set_function(
        "unregister",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](sol::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string uid)        // GCOVR_EXCL_LINE (lambda entry artifact)
        -> sol::object {  // GCOVR_EXCL_LINE (gcov attributes no code to
                          // this line)
            auto* pm = shield::player::PlayerManager::global();
            if (pm == nullptr) return sol::make_object(state, sol::nil);
            auto sid = pm->unregister(uid);
            if (!sid.has_value()) return sol::make_object(state, sol::nil);
            return sol::make_object(state, *sid);
        });

    manager_tbl.set_function(      // GCOVR_EXCL_LINE (gcov continuation
        "get",                     // artifact)
        [](sol::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string uid)        // GCOVR_EXCL_LINE (lambda entry artifact)
        -> sol::object {  // GCOVR_EXCL_LINE (gcov attributes no code to
                          // this line)
            auto* pm = shield::player::PlayerManager::global();
            sol::state_view s(state);
            if (pm == nullptr) return sol::make_object(state, sol::nil);
            auto info = pm->get(uid);
            if (!info.has_value()) return sol::make_object(state, sol::nil);
            return sol::make_object(s, write_session(s, *info));
        });

    manager_tbl.set_function(
        "get_devices",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](sol::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string uid) -> sol::table {  // GCOVR_EXCL_LINE (gcov attributes
                                             // no code to this line)
            sol::state_view s(state);
            sol::table out = s.create_table();
            auto* pm = shield::player::PlayerManager::global();
            if (pm == nullptr) return out;
            int i = 1;
            for (const auto& d : pm->get_devices(uid)) {
                out[i++] = d;
            }
            return out;
        });

    manager_tbl.set_function(
        "set_state",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](sol::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string
               uid,  // GCOVR_EXCL_LINE (gcov attributes no code to this line)
           std::string state_name) -> bool {
            auto* pm = shield::player::PlayerManager::global();
            if (pm == nullptr) return false;
            shield::player::SessionState parsed =
                shield::player::SessionState::kConnecting;
            if (std::string(state_name) == "online")
                parsed = shield::player::SessionState::kOnline;
            else if (std::string(state_name) == "ready")
                parsed = shield::player::SessionState::kReady;
            else if (std::string(state_name) == "anonymous")
                parsed = shield::player::SessionState::kAnonymous;
            else if (std::string(state_name) == "spectator")
                parsed = shield::player::SessionState::kSpectator;
            else if (std::string(state_name) == "disconnected")
                parsed = shield::player::SessionState::kDisconnected;
            else if (std::string(state_name) == "authenticating")
                parsed = shield::player::SessionState::kAuthenticating;
            else if (std::string(state_name) != "connecting")
                return false;
            return pm->set_state(uid, parsed);
        });

    manager_tbl.set_function("size", [](sol::this_state state) -> std::size_t {
        auto* pm = shield::player::PlayerManager::global();
        return pm ? pm->size() : 0;
    });

    player["manager"] = manager_tbl;

    // ---- resolve / get: local-only (P0) ----
    player.set_function(
        "resolve",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](sol::this_state state,   // GCOVR_EXCL_LINE (lambda entry artifact)
           sol::object ref)         // GCOVR_EXCL_LINE (lambda entry artifact)
        -> sol::variadic_results {  // GCOVR_EXCL_LINE (gcov attributes no
                                    // code to this line)
            sol::state_view s(state);
            sol::variadic_results results;
            auto* pm = shield::player::PlayerManager::global();
            if (pm == nullptr) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(make_error(state, "module_unavailable",
                                             "player manager unavailable"));
                return results;
            }
            shield::player::PlayerRef player_ref;
            if (ref.is<PlayerRefBox>()) {
                const PlayerRefData& d = ref.as<const PlayerRefBox&>().data;
                player_ref.uid = d.uid;
                player_ref.node_id = d.node_id;
                player_ref.service_id = d.service_id;
                player_ref.epoch = d.epoch;
            } else if (ref.is<sol::table>()) {
                player_ref = player_ref_from_table(ref);
            } else {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(make_error(state, "invalid_player_ref",
                                             "ref must be a PlayerRef"));
                return results;
            }
            if (player_ref.uid.empty()) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(make_error(state, "invalid_player_ref",
                                             "ref.uid is required"));
                return results;
            }
            if (!player_ref.node_id.empty() &&
                player_ref.node_id != pm->node_id()) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(make_error(
                    state, "remote_resolve_unimplemented",
                    "remote resolve is not part of the P0 contract"));
                return results;
            }
            auto info = pm->resolve(player_ref);
            if (!info.has_value()) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "player_not_found",
                               "no local session for uid: " + player_ref.uid));
                return results;
            }
            results.push_back(sol::make_object(s, write_session(s, *info)));
            results.push_back(sol::make_object(s, sol::nil));
            return results;
        });

    player.set_function(
        "get",
        [manager_tbl](sol::this_state state, std::string uid) -> sol::object {
            sol::protected_function get = manager_tbl["get"];
            return get(uid);
        });

    player["defaults"] = impl["defaults"];
    player["Base"] = impl["Base"];

    shield["player"] = player;
}
#else
// Module compiled out: every shield.player.* entry reports
// module_unavailable (LAPI-011 前言) instead of being a nil field.
void register_player_stub_api(sol::table& shield, sol::state_view lua) {
    auto unavailable = lua.safe_script(
        "return function()\n"  // GCOVR_EXCL_LINE (safe_script chunk artifact)
        "  return nil, {code = 'module_unavailable', message = "
        "'shield_player is not enabled', retryable = false}\n"
        "end\n",
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::protected_function_result
               pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
    auto player = lua.create_table();
    for (const char* name : {"setup", "resolve", "get", "config", "now_ms",
                             "node_info", "stats"}) {
        player[name] = unavailable;
    }
    auto manager = lua.create_table();
    for (const char* name :
         {"admit", "register_session", "mark_disconnected", "mark_reconnected",
          "unregister", "get", "get_devices", "set_state", "size"}) {
        manager[name] = unavailable;
    }
    player["manager"] = manager;
    auto defaults = lua.create_table();
    for (const char* name : {"allow", "ready", "reconnect", "save"}) {
        defaults[name] = unavailable;
    }
    player["defaults"] = defaults;
    // Base stays a table (its sugar shape is meaningless without the
    // module), but calling Base.setup reports the same stable code.
    auto base = lua.create_table();
    base["setup"] = unavailable;
    player["Base"] = base;
    shield["player"] = player;
}
#endif

#ifdef SHIELD_ENABLE_SERVER
// -----------------------------------------------------------------------------
// shield_server (P0): C++ ServerManager state truth + this per-VM
// orchestration chunk holding the watcher callbacks. See runtime-server.md.
//
// The chunk runs once per VM. register_server_api runs before
// lua["shield"] is assigned; all shield.* references resolve at CALL time.
// The C++ watch registry only stores {watch_id, service_id} (OD-016: no
// sol::function in shield_server), so this chunk owns the callbacks and
// installs the M.on_server_state_change forwarder the C++ notify path
// (send_system dispatch) calls into.
//
// The chunk also publishes its api table as a per-VM global
// (__shield_server_impl). The C++ facade resolves it at CALL time from the
// invoking state and must never capture it in a C++ lambda: a captured
// sol::table outlives the register call (the lambda userdata lives until
// GC finalizes it), and dereferencing that reference from the finalizer
// races/aliases the VM teardown (the same dangling-reference class the
// sol-reference-coroutine anchor rule exists for — long-lived C++ holders
// never keep sol references; resolve at call time instead).
// -----------------------------------------------------------------------------
constexpr const char* kServerOrchestration = R"lua(
local impl = {watchers = {}, prev = nil, installed = false}

local api = {
    -- Store the callback and (once per VM) install the forwarder the
    -- system-message dispatch calls into. Re-watching an existing id is
    -- idempotent: the original callback is kept.
    attach = function(M, watch_id, fn)
        if impl.watchers[watch_id] then return true end
        impl.watchers[watch_id] = fn
        if not impl.installed then
            impl.installed = true
            local prev = rawget(M, 'on_server_state_change')
            if type(prev) == 'function' then impl.prev = prev end
            M.on_server_state_change = function(ctx, new_state)
                if impl.prev then
                    local ok, err = pcall(impl.prev, ctx, new_state)
                    if not ok then
                        shield.log.warn('on_server_state_change failed: ' ..
                                            tostring(err))
                    end
                end
                for _, watcher in pairs(impl.watchers) do
                    local wok, werr = pcall(watcher, ctx, new_state)
                    if not wok then
                        shield.log.warn('server watch callback failed: ' ..
                                            tostring(werr))
                    end
                end
            end
        end
        return true
    end,
    detach = function(watch_id)
        impl.watchers[watch_id] = nil
        return true
    end,
}

rawset(_G, '__shield_server_impl', api)
return api
)lua";

void register_server_api(sol::table& shield, LuaServiceManager* manager,
                         LuaRuntime* runtime) {
    sol::state_view lua(shield.lua_state());

    // Run the orchestration chunk once per VM; it publishes the impl table
    // as the per-VM __shield_server_impl global, which the watch/unwatch
    // facades resolve at CALL time (never captured: see the chunk comment).
    lua.safe_script(
        kServerOrchestration,
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::protected_function_result
               pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE

    auto server = lua.create_table();

    // ---- read-only runtime info ----
    server.set_function("state", [](sol::this_state state) -> sol::object {
        auto* sm = shield::server::ServerManager::global();
        if (sm == nullptr) {
            return sol::make_object(state, sol::nil);
        }
        return sol::make_object(state,
                                shield::server::server_state_name(sm->state()));
    });

    server.set_function("uptime", [](sol::this_state state) -> sol::object {
        auto* sm = shield::server::ServerManager::global();
        if (sm == nullptr) {
            return sol::make_object(state, sol::nil);
        }
        return sol::make_object(state, sm->uptime_seconds());
    });

    server.set_function("version", [](sol::this_state state) -> sol::object {
        auto* sm = shield::server::ServerManager::global();
        if (sm == nullptr) {
            return sol::make_object(state, sol::nil);
        }
        return sol::make_object(state, sm->version());
    });

    server.set_function("node_id", [](sol::this_state state) -> sol::object {
        auto* sm = shield::server::ServerManager::global();
        if (sm == nullptr) {
            return sol::make_object(state, sol::nil);
        }
        return sol::make_object(state, sm->node_id());
    });

    server.set_function("started_at", [](sol::this_state state) -> sol::object {
        auto* sm = shield::server::ServerManager::global();
        if (sm == nullptr) {
            return sol::make_object(state, sol::nil);
        }
        return sol::make_object(state, sm->started_at_ms());
    });

    server.set_function("config", [](sol::this_state state) -> sol::object {
        sol::state_view s(state);
        auto* sm = shield::server::ServerManager::global();
        if (sm == nullptr) {
            return sol::make_object(s, sol::nil);
        }
        const auto& c = sm->config();
        sol::table cfg = s.create_table();
        cfg["name"] = c.name;
        sol::table info = s.create_table();
        info["name"] = c.info_name;
        info["version"] = c.info_version;
        info["region"] = c.info_region;
        cfg["info"] = info;
        return sol::make_object(s, cfg);
    });

    // ---- state control ----
    server.set_function(
        "set_state",
        [](sol::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
           std::string name) -> sol::variadic_results {
            sol::state_view s(state);
            sol::variadic_results results;
            auto* sm = shield::server::ServerManager::global();
            if (sm == nullptr) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_server is not initialized"));
                return results;
            }
            shield::server::ServerState parsed;
            if (!shield::server::parse_server_state(name, &parsed)) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(make_error(state, "invalid_state",
                                             "unknown server state: " + name));
                return results;
            }
            std::string error;
            if (!sm->set_state(parsed, &error)) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "invalid_state_transition", error));
                return results;
            }
            results.push_back(sol::make_object(s, true));
            return results;
        });

    server.set_function(
        "shutdown",
        [](sol::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::object delay) -> sol::variadic_results {
            sol::state_view s(state);
            sol::variadic_results results;
            auto* sm = shield::server::ServerManager::global();
            if (sm == nullptr) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_server is not initialized"));
                return results;
            }
            // The delay must be a non-negative integer number of
            // milliseconds. Lua numbers are doubles, so check through the
            // double view: negative and fractional values are rejected
            // here (an unsigned integer view would wrap negative values).
            std::uint64_t delay_ms = 0;
            bool valid = false;
            if (delay.is<double>()) {
                const double raw = delay.as<double>();
                if (raw >= 0.0 && raw == std::floor(raw) &&
                    raw <= 9007199254740992.0) {
                    delay_ms = static_cast<std::uint64_t>(raw);
                    valid = true;
                }
            }
            if (!valid) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(make_error(
                    state, "invalid_argument",
                    "shutdown delay must be a non-negative integer ms"));
                return results;
            }
            std::string error;
            if (!sm->schedule_shutdown(delay_ms, &error)) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "shutdown_already_scheduled", error));
                return results;
            }
            results.push_back(sol::make_object(s, true));
            return results;
        });

    // ---- state watchers ----
    server.set_function(
        "watch",
        [manager, runtime](sol::this_state state,
                           sol::object cb) -> sol::variadic_results {
            sol::state_view s(state);
            sol::variadic_results results;
            auto* sm = shield::server::ServerManager::global();
            if (sm == nullptr) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_server is not initialized"));
                return results;
            }
            if (!cb.is<sol::function>()) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "invalid_argument",
                               "watch expects a callback function"));
                return results;
            }
            // Watchers register for the calling service; that requires a
            // dispatch context (on_init or a handler).
            const auto service_id = manager->current_service_id();
            if (service_id.empty()) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "invalid_argument",
                               "watch requires a service context (call it from "
                               "on_init or a handler)"));
                return results;
            }
            sol::table module_tbl = sol::nil;
            if (runtime) {
                // The calling service may still be spawning (an on_init
                // watcher): it is not in the registry yet, so prefer the
                // dispatch frame's VM before the registry lookup.
                auto vm = manager->current_service_vm();
                // GCOVR_EXCL_START (defensive: current_service_vm already
                // retries through the registry, so a null result here means
                // the service is gone and service_vm would return null too)
                if (!vm) {
                    vm = manager->service_vm(service_id);
                }
                // GCOVR_EXCL_STOP
                module_tbl = runtime->service_table(vm);
            }
            // GCOVR_EXCL_START (defensive: every dispatch context resolves
            // a vm whose module loaded — load failures never reach the
            // dispatch scope, and a null vm above leaves the nil sentinel)
            if (!module_tbl.valid()) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "invalid_argument",
                               "watch requires a loaded service module"));
                return results;
            }
            // GCOVR_EXCL_STOP
            const auto watch_id = sm->watch(service_id);
            // The chunk stores the callback and installs the forwarder. The
            // impl table is re-resolved from this state's globals on every
            // call (coroutines share them) instead of being captured by the
            // lambda: a captured sol reference would be dereferenced from
            // the GC finalizer that later destroys the lambda.
            sol::table impl = s.globals()["__shield_server_impl"];
            sol::protected_function attach = impl["attach"];
            attach(module_tbl, watch_id, cb);
            results.push_back(sol::make_object(s, watch_id));
            return results;
        });

    server.set_function(
        "unwatch",
        [](sol::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::object watch_id) -> sol::variadic_results {
            sol::state_view s(state);
            sol::variadic_results results;
            auto* sm = shield::server::ServerManager::global();
            if (sm == nullptr) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_server is not initialized"));
                return results;
            }
            std::uint64_t id = 0;
            if (watch_id.is<std::uint64_t>()) {
                id = watch_id.as<std::uint64_t>();
            } else if (watch_id.is<double>()) {
                id = static_cast<std::uint64_t>(watch_id.as<double>());
            }
            sm->unwatch(id);
            sol::table impl = s.globals()["__shield_server_impl"];
            sol::protected_function detach = impl["detach"];
            detach(id);
            results.push_back(sol::make_object(s, true));
            return results;
        });

    shield["server"] = server;
}
#else
// Module compiled out: every shield.server.* entry reports
// module_unavailable instead of being a nil field.
void register_server_stub_api(sol::table& shield, sol::state_view lua) {
    auto unavailable = lua.safe_script(
        "return function()\n"  // GCOVR_EXCL_LINE (safe_script chunk artifact)
        "  return nil, {code = 'module_unavailable', message = "
        "'shield_server is not enabled', retryable = false}\n"
        "end\n",
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::protected_function_result
               pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
    auto server = lua.create_table();
    for (const char* name :
         {"state", "uptime", "version", "node_id", "started_at", "config",
          "set_state", "shutdown", "watch", "unwatch"}) {
        server[name] = unavailable;
    }
    shield["server"] = server;
}
#endif

#ifdef SHIELD_ENABLE_GLOBAL
// -----------------------------------------------------------------------------
// shield_global (P0): global data + local cache, locks, leaderboards,
// queues and the scheduler over the C++ GlobalManager store.
//
// Waiting entries (lock acquire, queue pop with timeout) MUST yield the
// running coroutine instead of blocking the actor thread — a yield can
// never cross a C++ call frame, so the wait loops live in this Lua chunk
// (never inside a C++ lambda) and only the non-blocking primitives are
// C++ functions injected up front as __shield_global_primitives.
// -----------------------------------------------------------------------------
constexpr const char* kGlobalOrchestration = R"lua(
local impl = {sched = {}, sched_prev = nil, sched_installed = false}
local prim = rawget(_G, '__shield_global_primitives')

-- Wait until try_fn() returns true. timeout_ms nil (or < 0) = single
-- attempt; 0 = poll until success; n > 0 = bounded wait. Uses
-- shield.sleep (coroutine-aware) so actor threads never block.
local function wait_for(try_fn, timeout_ms, retry_ms)
  if try_fn() then return true end
  if timeout_ms == nil or timeout_ms < 0 then return false end
  local deadline = shield.now() + timeout_ms
  while true do
    local remaining = deadline - shield.now()
    if remaining <= 0 then return false end
    shield.sleep(math.min(retry_ms, remaining))
    if try_fn() then return true end
  end
end

local function timeout_error(what, timeout_ms)
  return {code = 'timeout',
          message = what .. ' timed out after ' .. tostring(timeout_ms) .. 'ms',
          retryable = true}
end

-- ---- exclusive locks (mutex / spinlock / distributed_mutex) ----
local function make_exclusive(name, opts, registry)
  opts = opts or {}
  local lock = {
    name = name,
    ttl = opts.ttl or 0,
    retry = opts.retry or 20,
    _owner = registry .. ':' .. name .. ':' .. tostring({}),
  }
  function lock:try_acquire()
    return prim.lock_try(registry, name, self._owner, self.ttl)
  end
  function lock:release()
    return prim.lock_release(registry, name, self._owner)
  end
  function lock:extend(ttl)
    return prim.lock_extend(registry, name, self._owner, ttl or self.ttl)
  end
  function lock:owner()
    return prim.lock_info(registry, name)
  end
  function lock:ttl()
    local info = prim.lock_info(registry, name)
    if not info or not info.exists then return -1 end
    return info.ttl_remaining or -1
  end
  function lock:acquire(timeout)
    local waited = timeout or 0
    local ok = wait_for(function() return self:try_acquire() end, waited,
                        self.retry)
    if ok then return true end
    return false, timeout_error('lock acquire', waited)
  end
  function lock:with(fn)
    local ok, err = self:acquire()
    if not ok then return false, err end
    local results = table.pack(pcall(fn))
    self:release()
    if results[1] then
      return true, table.unpack(results, 2, results.n)
    end
    return false, results[2]
  end
  return lock
end

-- ---- reader/writer guards ----
local function make_guard(name, opts, kind, owner)
  local guard = {retry = (opts and opts.retry) or 20, _owner = owner}
  local try, release
  if kind == 'write' then
    try = function(ttl)
      return prim.rw_write_try(name, owner, ttl or (opts and opts.ttl) or 0)
    end
    release = function() return prim.rw_write_release(name, owner) end
  else
    try = function() return prim.rw_read_try(name, owner) end
    release = function() return prim.rw_read_release(name, owner) end
  end
  function guard:try_acquire() return try() end
  function guard:release() return release() end
  function guard:acquire(timeout)
    local waited = timeout or 0
    local ok = wait_for(function() return try() end, waited, self.retry)
    if ok then return true end
    return false, timeout_error('lock acquire', waited)
  end
  function guard:with(fn)
    local ok, err = self:acquire()
    if not ok then return false, err end
    local results = table.pack(pcall(fn))
    release()
    if results[1] then
      return true, table.unpack(results, 2, results.n)
    end
    return false, results[2]
  end
  return guard
end

local function make_rwlock(name, opts)
  return {
    name = name,
    read_lock = function(self)
      return make_guard(name, opts, 'read', 'r:' .. name .. ':' .. tostring({}))
    end,
    write_lock = function(self)
      return make_guard(name, opts, 'write', 'w:' .. name .. ':' .. tostring({}))
    end,
  }
end

-- ---- queue pop with timeout (single attempt / bounded wait) ----
local function pop_with_wait(pop_now, timeout)
  local msg = false
  local got = wait_for(function()
    local m = pop_now()
    if m ~= nil then msg = m return true end
    return false
  end, timeout, 10)
  if not got then return nil end
  return prim.decode(msg)
end

-- ---- queues ----
local function make_queue(name, opts)
  local q = {name = name}
  function q:push(v)
    prim.queue_push(name, prim.encode(v))
    return true
  end
  function q:push_batch(arr)
    for _, v in ipairs(arr) do prim.queue_push(name, prim.encode(v)) end
    return true
  end
  function q:pop(timeout)
    return pop_with_wait(function() return prim.queue_pop_now(name) end,
                         timeout)
  end
  function q:length() return prim.queue_length(name) end
  function q:purge()
    prim.queue_purge(name)
    return true
  end
  return q
end

local function make_delay_queue(name, opts)
  local q = {name = name}
  function q:push(v, delay)
    prim.delay_push(name, prim.encode(v), delay)
    return true
  end
  function q:push_at(v, at)
    -- Contract timestamps are seconds (os.time()), the store keeps ms.
    prim.delay_push_at(name, prim.encode(v), at * 1000)
    return true
  end
  function q:pop(timeout)
    return pop_with_wait(function() return prim.delay_pop_now(name) end,
                         timeout)
  end
  function q:pending() return prim.delay_pending(name) end
  function q:ready() return prim.delay_ready(name) end
  function q:purge()
    prim.delay_purge(name)
    return true
  end
  return q
end

local function make_reliable_queue(name, opts)
  opts = opts or {}
  prim.rel_configure(name, opts.max_retries or 3)
  local q = {name = name}
  function q:push(v)
    prim.rel_push(name, prim.encode(v))
    return true
  end
  function q:pop(timeout)
    local id, payload
    local got = wait_for(function()
      local i, p = prim.rel_pop_now(name)
      if i ~= nil then id = i payload = p return true end
      return false
    end, timeout, 10)
    if not got then return nil end
    local handle = {
      id = id,
      ack = function(self2) return prim.rel_ack(name, id) end,
      nack = function(self2, retry)
        return prim.rel_nack(name, id, retry or 0)
      end,
    }
    return prim.decode(payload), handle
  end
  function q:dead_letter()
    return {
      range = function(self2, from, to)
        local rows = prim.rel_dead_range(name, from, to or from)
        local out = {}
        for i, row in ipairs(rows) do out[i] = prim.decode(row.payload) end
        return out
      end,
      size = function(self2) return prim.rel_dead_size(name) end,
      purge = function(self2)
        prim.rel_dead_purge(name)
        return true
      end,
    }
  end
  return q
end

local function make_priority_queue(name, opts)
  local q = {name = name}
  -- Contract priorities: a smaller value pops first (urgent=1, normal=5,
  -- low=10); the default is the documented normal level.
  function q:push(v, priority)
    prim.priority_push(name, prim.encode(v), priority or 5)
    return true
  end
  function q:pop(timeout)
    return pop_with_wait(function() return prim.priority_pop_now(name) end,
                         timeout)
  end
  function q:length() return prim.priority_length(name) end
  function q:purge()
    prim.priority_purge(name)
    return true
  end
  return q
end

-- Broadcast: the store keeps bounded history + per-group cursors; the
-- callbacks themselves live in this VM (C++ never holds Lua refs). A
-- group that misses pushes catches up out of the history on its next
-- subscribe; cross-process live fan-out is the Phase 2+ Pub/Sub shape.
local function make_broadcast_queue(name, opts)
  opts = opts or {}
  local max_history = tonumber(opts.max_history)
  if max_history and max_history > 0 then
    prim.broadcast_configure(name, math.floor(max_history))
  end
  local q = {name = name, _subs = {}}
  local function deliver(group, msg, seq)
    local sub = q._subs[group]
    if not sub then return end  -- the group unsubscribed mid-dispatch
    local ok, err = pcall(sub.fn, msg)
    if not ok then
      shield.log.warn('broadcast callback failed: ' .. tostring(err))
    end
    prim.broadcast_commit(name, group, seq)
  end
  function q:push(v)
    local payload = prim.encode(v)
    local seq = prim.broadcast_push(name, payload)
    local msg = prim.decode(payload)
    for group in pairs(q._subs) do deliver(group, msg, seq) end
    return true
  end
  function q:subscribe(group, fn)
    if type(group) ~= 'string' or type(fn) ~= 'function' then
      return nil, {code = 'invalid_argument',
                   message = 'subscribe(group, function) required',
                   retryable = false}
    end
    -- Attach first: a new group starts at the head (no retro delivery);
    -- a known cursor catches up below.
    prim.broadcast_attach(name, group)
    q._subs[group] = {fn = fn}
    -- One offline catch-up pass. Pushes racing inside the replay (from a
    -- re-entrant callback) already delivered through q:push above.
    local rows, last = prim.broadcast_since(name, group)
    if last > 0 then
      for i = 1, #rows do
        local ok, err = pcall(fn, prim.decode(rows[i]))
        if not ok then
          shield.log.warn('broadcast replay failed: ' .. tostring(err))
        end
      end
      prim.broadcast_commit(name, group, last)
    end
    return true
  end
  function q:unsubscribe(group)
    q._subs[group] = nil
    return true
  end
  function q:history() return prim.broadcast_history_size(name) end
  function q:groups() return prim.broadcast_group_count(name) end
  function q:purge()
    prim.broadcast_purge(name)
    return true
  end
  return q
end

-- ---- rate limiter bounded wait (polls allow like queue pops) ----
impl.attach_rate_wait = function(limiter)
  limiter.wait = function(self, key, timeout)
    local ok = wait_for(function() return self:allow(key) end, timeout, 10)
    if ok then return true end
    return false, timeout_error('rate limit wait', timeout or 0)
  end
  return limiter
end

-- ---- scheduler callback registry ----
impl.attach_sched = function(M, task, fn)
  impl.sched[task] = fn
  if not impl.sched_installed then
    impl.sched_installed = true
    local prev = rawget(M, 'on_scheduler_task')
    if type(prev) == 'function' then impl.sched_prev = prev end
    M.on_scheduler_task = function(ctx, task)
      if impl.sched_prev then
        local ok, err = pcall(impl.sched_prev, ctx, task)
        if not ok then
          shield.log.warn('on_scheduler_task failed: ' .. tostring(err))
        end
      end
      local fn2 = impl.sched[task]
      if fn2 then
        local ok2, err2 = pcall(fn2, ctx)
        if not ok2 then
          shield.log.warn('scheduler task failed: ' .. tostring(err2))
        end
      else
        shield.log.warn('scheduler task has no callback: ' .. tostring(task))
      end
    end
  end
  return true
end
impl.detach_sched = function(task) impl.sched[task] = nil return true end
impl.sched_count = function()
  local n = 0
  for _ in pairs(impl.sched) do n = n + 1 end
  return n
end

local api = {
  make_mutex = function(name, opts) return make_exclusive(name, opts, 'mutex') end,
  make_spinlock = function(name, opts)
    return make_exclusive(name, opts, 'spinlock')
  end,
  make_rwlock = make_rwlock,
  make_queue = make_queue,
  make_delay_queue = make_delay_queue,
  make_priority_queue = make_priority_queue,
  make_broadcast_queue = make_broadcast_queue,
  make_reliable_queue = make_reliable_queue,
  pop_with_wait = pop_with_wait,
  attach_rate_wait = impl.attach_rate_wait,
  attach_sched = impl.attach_sched,
  detach_sched = impl.detach_sched,
  sched_count = impl.sched_count,
}
rawset(_G, '__shield_global_impl', api)
return api
)lua";

void register_global_api(sol::table& shield, LuaServiceManager* manager,
                         LuaRuntime* runtime) {
    sol::state_view lua(shield.lua_state());

    // Non-blocking primitives the orchestration chunk builds the waiting
    // API on. Created per VM, then handed to the chunk; C++ never keeps a
    // reference past this registration (call-time resolution only).
    sol::table prim = lua.create_table();
    auto* gm = shield::global::GlobalManager::global();
    auto lock_try = [gm](const std::string& registry, const std::string& name,
                         const std::string& owner, double ttl) {
        return gm->mutex_acquire(registry, name, owner,
                                 static_cast<std::uint64_t>(ttl)) ==
               shield::global::LockStatus::kOk;
    };
    prim.set_function("lock_try", lock_try);
    prim.set_function("lock_release",
                      [gm](const std::string& registry, const std::string& name,
                           const std::string& owner) {
                          return gm->mutex_release(registry, name, owner) ==
                                 shield::global::LockStatus::kOk;
                      });
    prim.set_function("lock_extend",
                      [gm](const std::string& registry, const std::string& name,
                           const std::string& owner, double ttl) {
                          return gm->mutex_extend(
                              registry, name, owner,
                              static_cast<std::uint64_t>(ttl));
                      });
    prim.set_function(
        "lock_info",
        [gm](sol::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& registry, const std::string& name) {
            sol::state_view s(state);
            const auto info = gm->mutex_info(registry, name);
            if (!info.exists) {
                return sol::make_object(s, sol::nil);
            }
            sol::table out = s.create_table();
            out["exists"] = true;
            out["owner"] = info.owner;
            out["count"] = info.count;
            out["acquired_at"] = info.acquired_at_ms;
            out["ttl_remaining"] = info.ttl_remaining_ms;
            return sol::make_object(s, out);
        });
    prim.set_function(
        "rw_write_try",
        [gm](const std::string& name, const std::string& owner, double ttl) {
            return gm->rw_write_acquire(name, owner,
                                        static_cast<std::uint64_t>(ttl)) ==
                   shield::global::LockStatus::kOk;
        });
    prim.set_function("rw_write_release",
                      [gm](const std::string& name, const std::string& owner) {
                          return gm->rw_write_release(name, owner) ==
                                 shield::global::LockStatus::kOk;
                      });
    prim.set_function(
        "rw_write_extend",
        [gm](const std::string& name, const std::string& owner, double ttl) {
            return gm->rw_write_extend(name, owner,
                                       static_cast<std::uint64_t>(ttl));
        });
    prim.set_function("rw_read_try",
                      [gm](const std::string& name, const std::string& owner) {
                          return gm->rw_read_acquire(name, owner) ==
                                 shield::global::LockStatus::kOk;
                      });
    prim.set_function("rw_read_release",
                      [gm](const std::string& name, const std::string& owner) {
                          return gm->rw_read_release(name, owner) ==
                                 shield::global::LockStatus::kOk;
                      });
    prim.set_function("queue_push", [gm](const std::string& name,
                                         const std::string& payload) {
        gm->queue_push(name, payload);
    });
    prim.set_function(
        "queue_pop_now",
        [gm](sol::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& name) -> sol::object {
            sol::state_view s(state);
            std::string payload;
            if (!gm->queue_pop(name, &payload)) {
                return sol::make_object(s, sol::nil);
            }
            return sol::make_object(s, payload);
        });
    prim.set_function("queue_length", [gm](const std::string& name) {
        return gm->queue_length(name);
    });
    prim.set_function("queue_purge",
                      [gm](const std::string& name) { gm->queue_purge(name); });
    prim.set_function(
        "delay_push", [gm](const std::string& name, const std::string& payload,
                           double delay) {
            gm->delay_push(name, payload, static_cast<std::uint64_t>(delay));
        });
    prim.set_function(
        "delay_push_at",
        [gm](const std::string& name, const std::string& payload, double at) {
            gm->delay_push_at(name, payload, static_cast<std::uint64_t>(at));
        });
    prim.set_function(
        "delay_pop_now",
        [gm](sol::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& name) -> sol::object {
            sol::state_view s(state);
            std::string payload;
            if (!gm->delay_pop(name, &payload)) {
                return sol::make_object(s, sol::nil);
            }
            return sol::make_object(s, payload);
        });
    prim.set_function("delay_pending", [gm](const std::string& name) {
        return gm->delay_pending(name);
    });
    prim.set_function("delay_ready", [gm](const std::string& name) {
        return gm->delay_ready(name);
    });
    prim.set_function("delay_purge",
                      [gm](const std::string& name) { gm->delay_purge(name); });
    prim.set_function("priority_push", [gm](const std::string& name,
                                            const std::string& payload,
                                            double priority) {
        gm->priority_push(name, payload, static_cast<std::int64_t>(priority));
    });
    prim.set_function(
        "priority_pop_now",
        [gm](sol::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& name) -> sol::object {
            sol::state_view s(state);
            std::string payload;
            if (!gm->priority_pop(name, &payload)) {
                return sol::make_object(s, sol::nil);
            }
            return sol::make_object(s, payload);
        });
    prim.set_function("priority_length", [gm](const std::string& name) {
        return gm->priority_length(name);
    });
    prim.set_function("priority_purge", [gm](const std::string& name) {
        gm->priority_purge(name);
    });
    prim.set_function("broadcast_configure", [gm](const std::string& name,
                                                  double max_history) {
        gm->broadcast_configure(name, static_cast<std::size_t>(max_history));
    });
    prim.set_function("broadcast_push", [gm](const std::string& name,
                                             const std::string& payload) {
        return gm->broadcast_push(name, payload);
    });
    prim.set_function("broadcast_attach",
                      [gm](const std::string& name, const std::string& group) {
                          return gm->broadcast_attach(name, group);
                      });
    prim.set_function(
        "broadcast_since",
        [gm](sol::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& name,
             const std::string& group) -> std::tuple<sol::object, sol::object> {
            sol::state_view s(state);
            std::vector<std::string> rows;
            const std::uint64_t last = gm->broadcast_since(name, group, &rows);
            sol::table out = s.create_table();
            for (std::size_t i = 0; i < rows.size(); ++i) {
                out[i + 1] = rows[i];
            }
            return {sol::make_object(s, out), sol::make_object(s, last)};
        });
    prim.set_function(
        "broadcast_commit",
        [gm](const std::string& name, const std::string& group, double seq) {
            gm->broadcast_commit(name, group, static_cast<std::uint64_t>(seq));
        });
    prim.set_function("broadcast_history_size", [gm](const std::string& name) {
        return gm->broadcast_history_size(name);
    });
    prim.set_function("broadcast_group_count", [gm](const std::string& name) {
        return gm->broadcast_group_count(name);
    });
    prim.set_function("broadcast_purge", [gm](const std::string& name) {
        gm->broadcast_purge(name);
    });
    prim.set_function(
        "rel_configure", [gm](const std::string& name, double max_retries) {
            gm->reliable_configure(name, static_cast<int>(max_retries));
        });
    prim.set_function(
        "rel_push", [gm](const std::string& name, const std::string& payload) {
            gm->reliable_push(name, payload);
        });
    prim.set_function(
        "rel_pop_now",
        [gm](sol::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& name) -> std::tuple<sol::object, sol::object> {
            sol::state_view s(state);
            shield::global::ReliableDelivery delivery;
            if (!gm->reliable_pop(name, &delivery)) {
                return {sol::make_object(s, sol::nil),
                        sol::make_object(s, sol::nil)};
            }
            return {sol::make_object(s, delivery.delivery_id),
                    sol::make_object(s, delivery.payload)};
        });
    prim.set_function("rel_ack", [gm](const std::string& name,
                                      double delivery_id) {
        return gm->reliable_ack(name, static_cast<std::uint64_t>(delivery_id));
    });
    prim.set_function("rel_nack", [gm](const std::string& name,
                                       double delivery_id, double retry) {
        switch (gm->reliable_nack(name, static_cast<std::uint64_t>(delivery_id),
                                  static_cast<std::uint64_t>(retry))) {
            case shield::global::NackResult::kRequeued:
                return "requeued";
            case shield::global::NackResult::kDead:
                return "dead";
            case shield::global::NackResult::kNotFound:
                break;
        }
        return "not_found";
    });
    prim.set_function(
        "rel_dead_range",
        [gm](sol::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
             const std::string& name, double from, double to) -> sol::object {
            sol::state_view s(state);
            auto entries =
                gm->reliable_dead_range(name, static_cast<std::size_t>(from),
                                        static_cast<std::size_t>(to));
            sol::table out = s.create_table();
            for (const auto& entry : entries) {
                sol::table row = s.create_table();
                row["id"] = entry.delivery_id;
                row["payload"] = entry.payload;
                row["retries"] = entry.retries;
                out.add(row);
            }
            return out;
        });
    prim.set_function("rel_dead_size", [gm](const std::string& name) {
        return gm->reliable_dead_size(name);
    });
    prim.set_function("rel_dead_purge", [gm](const std::string& name) {
        gm->reliable_dead_purge(name);
    });
    prim.set_function(
        "encode", [](sol::object value) { return lua_to_json(value).dump(); });
    prim.set_function(
        "decode",
        [](sol::this_state state,  // GCOVR_EXCL_LINE (gcov clone artifact)
           const std::string& text) -> sol::object {
            sol::state_view s(state);
            auto parsed = nlohmann::json::parse(text, nullptr, false);
            if (parsed.is_discarded()) {
                return sol::make_object(s, sol::nil);
            }
            return json_to_lua(s, parsed);
        });
    lua["__shield_global_primitives"] = prim;

    // Run the orchestration chunk once per VM.
    lua.safe_script(
        kGlobalOrchestration,
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::protected_function_result
               pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE

    // Resolve the impl table at CALL time (never captured: same rule as
    // the server orchestration chunk).
    auto impl_table = [lua]() {
        sol::table impl = lua.globals()["__shield_global_impl"];
        return impl;
    };

    // ---- shield.global(): global data + local cache ----
    shield.set_function(
        "global", [lua](sol::this_state state) -> sol::variadic_results {
            sol::state_view s(state);
            sol::variadic_results results;
            auto* mgr = shield::global::GlobalManager::global();
            if (mgr == nullptr) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_global is not initialized"));
                return results;
            }
            sol::table g = s.create_table();
            g.set_function("set", [mgr](sol::object /*self*/, sol::object key,
                                        sol::object value,
                                        sol::optional<double> ttl) {
                mgr->data_set(key.as<std::string>(), lua_to_json(value).dump(),
                              ttl ? static_cast<std::uint64_t>(*ttl) : 0);
                return true;
            });
            g.set_function(
                "get",
                [mgr](sol::this_state state,  // GCOVR_EXCL_LINE
                      sol::object /*self*/, sol::object key) -> sol::object {
                    sol::state_view s(state);
                    std::string value;
                    if (!mgr->data_get(key.as<std::string>(), &value)) {
                        return sol::make_object(s, sol::nil);
                    }
                    auto parsed = nlohmann::json::parse(value, nullptr, false);
                    if (parsed.is_discarded()) {
                        return sol::make_object(s, value);
                    }
                    return json_to_lua(s, parsed);
                });
            g.set_function("delete", [mgr](sol::object, sol::object key) {
                return mgr->data_delete(key.as<std::string>());
            });
            g.set_function(
                "incr",
                [mgr](sol::this_state state,  // GCOVR_EXCL_LINE
                      sol::object /*self*/, sol::object key,
                      sol::optional<double> delta) -> sol::variadic_results {
                    sol::state_view s(state);
                    sol::variadic_results results;
                    std::int64_t out = 0;
                    std::string error;
                    if (!mgr->data_incr_by(
                            key.as<std::string>(),
                            delta ? static_cast<std::int64_t>(*delta) : 1, &out,
                            &error)) {
                        results.push_back(sol::make_object(s, sol::nil));
                        results.push_back(
                            make_error(state, "invalid_value", error));
                        return results;
                    }
                    results.push_back(sol::make_object(s, out));
                    return results;
                });
            g.set_function(
                "decr",
                [mgr](sol::this_state state,  // GCOVR_EXCL_LINE
                      sol::object /*self*/, sol::object key,
                      sol::optional<double> delta) -> sol::variadic_results {
                    sol::state_view s(state);
                    sol::variadic_results results;
                    std::int64_t out = 0;
                    std::string error;
                    if (!mgr->data_incr_by(
                            key.as<std::string>(),
                            delta ? -static_cast<std::int64_t>(*delta) : -1,
                            &out, &error)) {
                        results.push_back(sol::make_object(s, sol::nil));
                        results.push_back(
                            make_error(state, "invalid_value", error));
                        return results;
                    }
                    results.push_back(sol::make_object(s, out));
                    return results;
                });
            g.set_function(
                "mset",
                [mgr](sol::this_state state,  // GCOVR_EXCL_LINE
                      sol::object /*self*/, sol::table kvs,
                      sol::optional<double> ttl) -> sol::variadic_results {
                    sol::state_view s(state);
                    sol::variadic_results results;
                    std::vector<std::pair<std::string, std::string>> pairs;
                    for (auto& [k, v] : kvs) {
                        if (k.is<std::string>()) {
                            pairs.emplace_back(k.as<std::string>(),
                                               lua_to_json(v).dump());
                        }
                    }
                    std::string error;
                    if (!mgr->data_mset(
                            pairs, ttl ? static_cast<std::uint64_t>(*ttl) : 0,
                            &error)) {
                        results.push_back(sol::make_object(s, sol::nil));
                        results.push_back(
                            make_error(state, "invalid_argument", error));
                        return results;
                    }
                    results.push_back(sol::make_object(s, true));
                    return results;
                });
            g.set_function(
                "mget",
                [mgr](sol::this_state state,  // GCOVR_EXCL_LINE
                      sol::object /*self*/,
                      sol::variadic_args args) -> sol::object {
                    sol::state_view s(state);
                    sol::table out = s.create_table();
                    for (const auto& arg : args) {
                        std::string value;
                        if (mgr->data_get(arg.as<std::string>(), &value)) {
                            auto parsed =
                                nlohmann::json::parse(value, nullptr, false);
                            if (!parsed.is_discarded()) {
                                out.add(json_to_lua(s, parsed));
                                continue;
                            }
                            out.add(sol::make_object(s, value));
                        } else {
                            out.add(sol::make_object(s, sol::nil));
                        }
                    }
                    return out;
                });
            g.set_function(
                "get_cached",
                [mgr](sol::this_state state,  // GCOVR_EXCL_LINE
                      sol::object /*self*/, sol::object key,
                      sol::optional<double> ttl) -> sol::object {
                    sol::state_view s(state);
                    std::string value;
                    if (!mgr->cache_get(
                            key.as<std::string>(),
                            ttl ? static_cast<std::uint64_t>(*ttl) : 0,
                            &value)) {
                        return sol::make_object(s, sol::nil);
                    }
                    auto parsed = nlohmann::json::parse(value, nullptr, false);
                    if (parsed.is_discarded()) {
                        return sol::make_object(s, value);
                    }
                    return json_to_lua(s, parsed);
                });
            g.set_function("invalidate", [mgr](sol::object, sol::object key) {
                mgr->cache_invalidate(key.as<std::string>());
                return true;
            });
            results.push_back(sol::make_object(s, g));
            return results;
        });

    // ---- lock factories (waiting semantics live in the chunk) ----
    auto lock_factory = [lua, impl_table](sol::this_state state,
                                          const char* maker, sol::object name,
                                          sol::optional<sol::table> opts) {
        sol::state_view s(state);
        auto* mgr = shield::global::GlobalManager::global();
        if (mgr == nullptr) {
            return std::make_tuple(
                sol::make_object(s, sol::nil),
                sol::make_object(
                    s, make_error(state, "module_unavailable",
                                  "shield_global is not initialized")));
        }
        (void)mgr;
        sol::table impl = impl_table();
        sol::protected_function make = impl[maker];
        sol::protected_function_result result =
            name.is<std::string>() && opts.has_value()
                ? make(name.as<std::string>(), *opts)
                : make(name.is<std::string>() ? name.as<std::string>() : "");
        if (!result.valid()) {
            return std::make_tuple(
                sol::make_object(s, sol::nil),
                sol::make_object(s, make_error(state, "invalid_argument",
                                               "invalid lock arguments")));
        }
        // Unwrap the pfr explicitly: make_object on the whole result does
        // not reliably push the callee's first return value here.
        sol::object lock_obj = result.get<sol::object>(0);
        return std::make_tuple(std::move(lock_obj),
                               sol::make_object(s, sol::nil));
    };
    shield.set_function(
        "mutex", [lock_factory](sol::this_state state, sol::object name,
                                sol::optional<sol::table> opts) {
            return lock_factory(state, "make_mutex", name, opts);
        });
    shield.set_function(
        "spinlock", [lock_factory](sol::this_state state, sol::object name,
                                   sol::optional<sol::table> opts) {
            return lock_factory(state, "make_spinlock", name, opts);
        });
    shield.set_function(
        "rwlock", [lock_factory](sol::this_state state, sol::object name,
                                 sol::optional<sol::table> opts) {
            return lock_factory(state, "make_rwlock", name, opts);
        });
    shield.set_function("distributed_mutex",
                        [lock_factory](sol::this_state state, sol::object name,
                                       sol::optional<sol::table> opts) {
                            // P0: the distributed twin rides the in-process
                            // backend (the cross-process seam is the manager
                            // itself).
                            return lock_factory(state, "make_mutex", name,
                                                opts);
                        });
    shield.set_function("distributed_rwlock",
                        [lock_factory](sol::this_state state, sol::object name,
                                       sol::optional<sol::table> opts) {
                            return lock_factory(state, "make_rwlock", name,
                                                opts);
                        });

    // ---- shield.rank(name) ----
    shield.set_function(
        "rank",
        [lua](sol::this_state state,
              sol::object board_obj) -> sol::variadic_results {
            sol::state_view s(state);
            sol::variadic_results results;
            auto* mgr = shield::global::GlobalManager::global();
            if (mgr == nullptr) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_global is not initialized"));
                return results;
            }
            if (!board_obj.is<std::string>()) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(make_error(state, "invalid_argument",
                                             "rank requires a board name"));
                return results;
            }
            const std::string board = board_obj.as<std::string>();
            sol::table rank = s.create_table();
            rank.set_function(
                "update", [mgr, board](sol::object /*self*/, std::string uid,
                                       double score) {
                    mgr->rank_update(board, uid, score);
                    return true;
                });
            rank.set_function("mupdate", [mgr, board](sol::object /*self*/,
                                                      sol::table updates) {
                std::vector<std::pair<std::string, double>> batch;
                for (auto& [uid, score] : updates) {
                    if (uid.is<std::string>() && score.is<double>()) {
                        batch.emplace_back(uid.as<std::string>(),
                                           score.as<double>());
                    }
                }
                mgr->rank_mupdate(board, batch);
                return true;
            });
            rank.set_function(
                "score",
                [mgr, board](sol::this_state state, sol::object /*self*/,
                             sol::object uid) -> sol::object {
                    sol::state_view s(state);
                    if (!uid.is<std::string>()) {
                        return sol::make_object(s, sol::nil);
                    }
                    auto value = mgr->rank_score(board, uid.as<std::string>());
                    return value ? sol::make_object(s, *value)
                                 : sol::make_object(s, sol::nil);
                });
            rank.set_function(
                "position",
                [mgr, board](sol::this_state state, sol::object /*self*/,
                             sol::object uid) -> sol::object {
                    sol::state_view s(state);
                    if (!uid.is<std::string>()) {
                        return sol::make_object(s, sol::nil);
                    }
                    auto value =
                        mgr->rank_position(board, uid.as<std::string>());
                    return value ? sol::make_object(s, *value)
                                 : sol::make_object(s, sol::nil);
                });
            auto entry_table = [](sol::state_view s,
                                  const shield::global::RankEntry& entry) {
                sol::table row = s.create_table();
                row["uid"] = entry.uid;
                row["score"] = entry.score;
                row["rank"] = entry.rank;
                return row;
            };
            rank.set_function(
                "top",
                [mgr, board, entry_table](sol::this_state state,
                                          sol::object /*self*/,
                                          double n) -> sol::object {
                    sol::state_view s(state);
                    sol::table out = s.create_table();
                    for (const auto& entry :
                         mgr->rank_top(board, static_cast<std::size_t>(n))) {
                        out.add(entry_table(s, entry));
                    }
                    return out;
                });
            rank.set_function(
                "range",
                [mgr, board, entry_table](sol::this_state state,
                                          sol::object /*self*/, double from,
                                          double to) -> sol::object {
                    sol::state_view s(state);
                    sol::table out = s.create_table();
                    for (const auto& entry : mgr->rank_range(
                             board, static_cast<std::uint64_t>(from),
                             static_cast<std::uint64_t>(to))) {
                        out.add(entry_table(s, entry));
                    }
                    return out;
                });
            rank.set_function(
                "range_by_score",
                [mgr, board, entry_table](sol::this_state state,
                                          sol::object /*self*/, double lo,
                                          double hi) -> sol::object {
                    sol::state_view s(state);
                    sol::table out = s.create_table();
                    for (const auto& entry :
                         mgr->rank_range_by_score(board, lo, hi)) {
                        out.add(entry_table(s, entry));
                    }
                    return out;
                });
            rank.set_function(
                "around",
                [mgr, board, entry_table](sol::this_state state,
                                          sol::object /*self*/, sol::object uid,
                                          double n) -> sol::object {
                    sol::state_view s(state);
                    if (!uid.is<std::string>()) {
                        return sol::make_object(s, sol::nil);
                    }
                    sol::table out = s.create_table();
                    const auto around =
                        mgr->rank_around(board, uid.as<std::string>(),
                                         static_cast<std::size_t>(n));
                    sol::table above = s.create_table();
                    for (const auto& entry : around.above) {
                        above.add(entry_table(s, entry));
                    }
                    sol::table below = s.create_table();
                    for (const auto& entry : around.below) {
                        below.add(entry_table(s, entry));
                    }
                    out["above"] = above;
                    out["target"] = around.target
                                        ? sol::make_object(
                                              s, entry_table(s, *around.target))
                                        : sol::make_object(s, sol::nil);
                    out["below"] = below;
                    return out;
                });
            rank.set_function("count", [mgr, board](sol::object /*self*/) {
                return mgr->rank_count(board);
            });
            rank.set_function(
                "remove", [mgr, board](sol::object /*self*/, std::string uid) {
                    return mgr->rank_remove(board, uid);
                });
            rank.set_function("clear", [mgr, board](sol::object /*self*/) {
                mgr->rank_clear(board);
                return true;
            });
            results.push_back(sol::make_object(s, rank));
            return results;
        });

    // ---- queue factories (waiting pop semantics live in the chunk) ----
    auto queue_factory = [lua, impl_table](sol::this_state state,
                                           const char* maker, sol::object name,
                                           sol::optional<sol::table> opts) {
        sol::state_view s(state);
        auto* mgr = shield::global::GlobalManager::global();
        if (mgr == nullptr) {
            return std::make_tuple(
                sol::make_object(s, sol::nil),
                sol::make_object(
                    s, make_error(state, "module_unavailable",
                                  "shield_global is not initialized")));
        }
        (void)mgr;
        sol::table impl = impl_table();
        sol::protected_function make = impl[maker];
        sol::protected_function_result result =
            opts.has_value() ? make(name, *opts) : make(name);
        if (!result.valid()) {
            return std::make_tuple(
                sol::make_object(s, sol::nil),
                sol::make_object(s, make_error(state, "invalid_argument",
                                               "invalid queue arguments")));
        }
        // Unwrap the pfr explicitly: make_object on the whole result does
        // not reliably push the callee's first return value here.
        sol::object queue_obj = result.get<sol::object>(0);
        return std::make_tuple(std::move(queue_obj),
                               sol::make_object(s, sol::nil));
    };
    shield.set_function(
        "queue", [queue_factory](sol::this_state state, sol::object name,
                                 sol::optional<sol::table> opts) {
            return queue_factory(state, "make_queue", name, opts);
        });
    shield.set_function(
        "delay_queue", [queue_factory](sol::this_state state, sol::object name,
                                       sol::optional<sol::table> opts) {
            return queue_factory(state, "make_delay_queue", name, opts);
        });
    shield.set_function("reliable_queue",
                        [queue_factory](sol::this_state state, sol::object name,
                                        sol::optional<sol::table> opts) {
                            return queue_factory(state, "make_reliable_queue",
                                                 name, opts);
                        });
    shield.set_function("priority_queue",
                        [queue_factory](sol::this_state state, sol::object name,
                                        sol::optional<sol::table> opts) {
                            return queue_factory(state, "make_priority_queue",
                                                 name, opts);
                        });
    shield.set_function("broadcast_queue",
                        [queue_factory](sol::this_state state, sol::object name,
                                        sol::optional<sol::table> opts) {
                            return queue_factory(state, "make_broadcast_queue",
                                                 name, opts);
                        });

    // ---- shield.scheduler() ----
    shield.set_function(
        "scheduler",
        [lua, manager,
         runtime](sol::this_state state) -> sol::variadic_results {
            sol::state_view s(state);
            sol::variadic_results results;
            auto* mgr = shield::global::GlobalManager::global();
            if (mgr == nullptr) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_global is not initialized"));
                return results;
            }
            auto register_task = [lua, manager, runtime](
                                     sol::this_state state, const char* type,
                                     sol::object name, sol::object schedule,
                                     sol::object cb) -> sol::variadic_results {
                sol::state_view s(state);
                sol::variadic_results results;
                auto* mgr2 = shield::global::GlobalManager::global();
                if (!name.is<std::string>() || name.as<std::string>().empty()) {
                    results.push_back(sol::make_object(s, sol::nil));
                    results.push_back(
                        make_error(state, "invalid_argument",
                                   "task name must be a non-empty string"));
                    return results;
                }
                if (!cb.is<sol::function>()) {
                    results.push_back(sol::make_object(s, sol::nil));
                    results.push_back(
                        make_error(state, "invalid_argument",
                                   "task expects a callback function"));
                    return results;
                }
                std::string schedule_text;
                if (schedule.is<std::string>()) {
                    schedule_text = schedule.as<std::string>();
                } else if (schedule.is<double>()) {
                    const double raw = schedule.as<double>();
                    if (raw < 0.0 || raw != std::floor(raw)) {
                        results.push_back(sol::make_object(s, sol::nil));
                        results.push_back(make_error(
                            state, "invalid_argument",
                            "schedule must be a non-negative integer ms"));
                        return results;
                    }
                    schedule_text =
                        std::to_string(static_cast<std::uint64_t>(raw));
                } else {
                    results.push_back(sol::make_object(s, sol::nil));
                    results.push_back(make_error(
                        state, "invalid_argument",
                        "schedule must be a cron string or an ms number"));
                    return results;
                }
                const std::string service_id = manager->current_service_id();
                if (service_id.empty()) {
                    results.push_back(sol::make_object(s, sol::nil));
                    results.push_back(make_error(
                        state, "invalid_argument",
                        "scheduler tasks require a service context (register "
                        "from on_init or a handler)"));
                    return results;
                }
                std::string error;
                if (!mgr2->sched_register(type, name.as<std::string>(),
                                          schedule_text, service_id, &error)) {
                    results.push_back(sol::make_object(s, sol::nil));
                    results.push_back(
                        make_error(state, "invalid_argument", error));
                    return results;
                }
                // Resolve the calling service's module table and store the
                // callback in the per-VM registry (same VM resolution as
                // the server watch facade).
                sol::table module_tbl = sol::nil;
                if (runtime) {
                    auto vm = manager->current_service_vm();
                    if (!vm) {
                        vm = manager->service_vm(service_id);
                    }
                    if (vm) {
                        module_tbl = runtime->service_table(vm);
                    }
                }
                if (!module_tbl.valid()) {
                    mgr2->sched_remove(name.as<std::string>());
                    results.push_back(sol::make_object(s, sol::nil));
                    results.push_back(make_error(
                        state, "invalid_argument",
                        "scheduler tasks require a loaded service module"));
                    return results;
                }
                sol::table impl = s.globals()["__shield_global_impl"];
                sol::protected_function attach = impl["attach_sched"];
                attach(module_tbl, name, cb);
                results.push_back(sol::make_object(s, true));
                return results;
            };
            sol::table sched = s.create_table();
            sched.set_function(
                "cron", [register_task](sol::this_state state,
                                        sol::object /*self*/, sol::object name,
                                        sol::object expr, sol::object cb,
                                        sol::optional<sol::table> /*opts*/) {
                    return register_task(state, "cron", name, expr, cb);
                });
            sched.set_function(
                "interval",
                [register_task](sol::this_state state, sol::object /*self*/,
                                sol::object name, sol::object ms,
                                sol::object cb,
                                sol::optional<sol::table> /*opts*/) {
                    return register_task(state, "interval", name, ms, cb);
                });
            sched.set_function(
                "once", [register_task](sol::this_state state,
                                        sol::object /*self*/, sol::object name,
                                        sol::object delay, sol::object cb,
                                        sol::optional<sol::table> /*opts*/) {
                    return register_task(state, "once", name, delay, cb);
                });
            sched.set_function(
                "get",
                [mgr](sol::this_state state,  // GCOVR_EXCL_LINE
                      sol::object, std::string name) -> sol::object {
                    sol::state_view s(state);
                    auto info = mgr->sched_get(name);
                    if (!info.has_value()) {
                        return sol::make_object(s, sol::nil);
                    }
                    sol::table out = s.create_table();
                    out["name"] = info->name;
                    out["type"] = info->type;
                    out["schedule"] = info->schedule;
                    out["next_run"] = info->next_run_ms;
                    out["last_run"] = info->last_run_ms;
                    out["run_count"] = info->run_count;
                    out["status"] = info->paused ? "paused"
                                    : info->done ? "done"
                                                 : "active";
                    return out;
                });
            sched.set_function("pause", [mgr](sol::object, std::string name) {
                return mgr->sched_pause(name);
            });
            sched.set_function("resume", [mgr](sol::object, std::string name) {
                return mgr->sched_resume(name);
            });
            sched.set_function(
                "remove", [lua, mgr](sol::this_state state, sol::object,
                                     std::string name) {
                    mgr->sched_remove(name);
                    sol::state_view s(state);
                    sol::table impl = s.globals()["__shield_global_impl"];
                    sol::protected_function detach = impl["detach_sched"];
                    detach(name);
                    return true;
                });
            sched.set_function("trigger", [mgr](sol::object, std::string name) {
                return mgr->sched_trigger(name);
            });
            results.push_back(sol::make_object(s, sched));
            return results;
        });

    // ---- shield.rate_limiter(name, opts) ----
    shield.set_function(
        "rate_limiter",
        [lua, impl_table](
            sol::this_state state, sol::object name_obj,
            sol::optional<sol::table> opts) -> sol::variadic_results {
            sol::state_view s(state);
            sol::variadic_results results;
            auto* mgr = shield::global::GlobalManager::global();
            if (mgr == nullptr) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "module_unavailable",
                               "shield_global is not initialized"));
                return results;
            }
            if (!name_obj.is<std::string>()) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(make_error(state, "invalid_argument",
                                             "rate_limiter requires a name"));
                return results;
            }
            const std::string name = name_obj.as<std::string>();
            shield::global::RateLimitConfig config;
            if (opts.has_value()) {
                sol::table o = *opts;
                config.rate = o.get_or("rate", 100.0);
                config.burst = o.get_or("burst", 200.0);
                config.sliding = o.get_or("sliding", false);
                config.window_ms =
                    static_cast<std::uint64_t>(o.get_or("window", 60000.0));
                config.max_requests =
                    static_cast<std::uint64_t>(o.get_or("max_requests", 100.0));
            }
            mgr->rate_limit_configure(name, config);
            sol::table limiter = s.create_table();
            limiter["name"] = name;
            limiter.set_function(
                "allow", [mgr, name](sol::object /*self*/, std::string key) {
                    return mgr->rate_limit_allow(name, key, 1.0).allowed;
                });
            limiter.set_function("remaining", [mgr, name](sol::object /*self*/,
                                                          std::string key) {
                return mgr->rate_limit_remaining(name, key);
            });
            // Bounded wait lives in the chunk (coroutine-aware sleep).
            sol::table impl = impl_table();
            sol::protected_function attach = impl["attach_rate_wait"];
            auto attached = attach(limiter);
            if (!attached.valid()) {
                results.push_back(sol::make_object(s, sol::nil));
                results.push_back(
                    make_error(state, "invalid_argument",
                               "rate limiter wait attach failed"));
                return results;
            }
            results.push_back(sol::make_object(s, limiter));
            return results;
        });
}
#else
// Module compiled out: every shield_global factory reports
// module_unavailable instead of being a nil field.
void register_global_stub_api(sol::table& shield, sol::state_view lua) {
    auto unavailable = lua.safe_script(
        "return function()\n"  // GCOVR_EXCL_LINE (safe_script chunk artifact)
        "  return nil, {code = 'module_unavailable', message = "
        "'shield_global is not enabled', retryable = false}\n"
        "end\n",
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::protected_function_result
               pfr)  // GCOVR_EXCL_LINE (gcov clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
    for (const char* name :
         {"global", "mutex", "rwlock", "spinlock", "distributed_mutex",
          "distributed_rwlock", "rank", "queue", "delay_queue",
          "priority_queue", "broadcast_queue", "reliable_queue", "scheduler",
          "rate_limiter"}) {
        shield[name] = unavailable;
    }
}
#endif

void register_http_api(sol::table& shield, LuaServiceManager* manager,
                       LuaRuntime* runtime) {
    sol::state_view lua(shield.lua_state());

    // =========================================================================
    // shield.http — HTTP 客户端（发请求）
    // =========================================================================
    auto http = lua.create_table();

    // Helper: convert HttpClientResponse to Lua table.
    // Auto-parses JSON body into `data` field when Content-Type is JSON.
    auto to_table =
        [](sol::state_view lua,
           const shield::net::HttpClientResponse& res) -> sol::table {
        sol::table result = lua.create_table();
        result["status"] = res.status_code;
        result["body"] = res.body;
        result["ok"] = res.ok();
        result["error"] = res.error;
        sol::table headers = lua.create_table();
        for (const auto& [k, v] : res.headers) {
            headers[k] = v;
        }
        result["headers"] = headers;

        // Auto-parse JSON response body into `data` field.
        if (!res.body.empty()) {
            auto ct_it = res.headers.find("Content-Type");
            if (ct_it == res.headers.end())
                ct_it = res.headers.find("content-type");
            bool is_json = false;
            if (ct_it != res.headers.end()) {
                is_json =
                    ct_it->second.find("application/json") != std::string::npos;
            }
            // Also try parsing if body starts with { or [
            if (!is_json && !res.body.empty() &&
                (res.body[0] == '{' || res.body[0] == '[')) {
                is_json = true;
            }
            if (is_json) {
                try {
                    auto json = nlohmann::json::parse(res.body);
                    result["data"] = json_to_lua(lua, json);
                } catch (...) {
                    // Not valid JSON, leave data as nil.
                }
            }
        }
        return result;
    };

    // Helper: parse options table into HttpClientOptions.
    auto parse_opts = [](sol::table opts,
                         shield::net::HttpClientOptions& options) {
        if (opts["method"].valid()) {
            options.method = opts["method"].get<std::string>();
        }
        if (opts["body"].valid()) {
            options.body = opts["body"].get<std::string>();
        }
        if (opts["timeout"].valid()) {
            options.timeout_seconds = opts["timeout"].get<int>();
        }
        if (opts["headers"].valid()) {
            sol::table hdrs = opts["headers"];
            for (auto& [k, v] : hdrs) {
                if (k.is<std::string>() && v.is<std::string>()) {
                    options.headers[k.as<std::string>()] = v.as<std::string>();
                }
            }
        }
        if (opts["auth_bearer"].valid()) {
            options.auth_bearer = opts["auth_bearer"].get<std::string>();
            options.headers["Authorization"] = "Bearer " + options.auth_bearer;
        }
        if (opts["auth_basic"].valid()) {
            sol::table basic = opts["auth_basic"];
            if (basic["user"].valid()) {
                options.auth_basic_user = basic["user"].get<std::string>();
            }
            if (basic["password"].valid()) {
                options.auth_basic_password =
                    basic["password"].get<std::string>();
            }
        }
        if (opts["proxy"].valid()) {
            options.proxy = opts["proxy"].get<std::string>();
        }
        if (opts["verify_ssl"].valid()) {
            options.verify_ssl = opts["verify_ssl"].get<bool>();
        }
        if (opts["ca_cert_path"].valid()) {
            options.ca_cert_path = opts["ca_cert_path"].get<std::string>();
        }
        if (opts["retry"].valid()) {
            options.retry_count = opts["retry"].get<int>();
        }
        if (opts["retry_delay"].valid()) {
            options.retry_delay_ms = opts["retry_delay"].get<int>();
        }
        if (opts["follow_redirects"].valid()) {
            options.follow_redirects = opts["follow_redirects"].get<bool>();
        }
        if (opts["max_redirects"].valid()) {
            options.max_redirects = opts["max_redirects"].get<int>();
        }
    };

    // shield.http.request(url, options) -> response_table
    // Full options: method, body, headers, timeout, auth_bearer,
    //   auth_basic={user,password}, proxy, verify_ssl, retry, retry_delay,
    //   follow_redirects, max_redirects
    http.set_function(
        "request",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);

            shield::net::HttpClientOptions options;
            options.url = url;

            if (opts) {
                parse_opts(*opts, options);
            }

            auto res = shield::net::HttpClient::request(options);
            return to_table(lua, res);
        });

    // Convenience: shield.http.get(url [, options]) -> response_table
    http.set_function(
        "get",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "GET";
            options.url = url;
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.post(url [, body] [, options]) -> response_table
    http.set_function(
        "post",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::optional<std::string> body,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "POST";
            options.url = url;
            options.body = body.value_or("");
            options.headers["Content-Type"] = "application/json";
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.put(url [, body] [, options]) -> response_table
    http.set_function(
        "put",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::optional<std::string> body,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PUT";
            options.url = url;
            options.body = body.value_or("");
            options.headers["Content-Type"] = "application/json";
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.delete(url [, options]) -> response_table
    http.set_function(
        "delete",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "DELETE";
            options.url = url;
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // Convenience: shield.http.patch(url [, body] [, options]) ->
    // response_table
    http.set_function(
        "patch",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::optional<std::string> body,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PATCH";
            options.url = url;
            options.body = body.value_or("");
            options.headers["Content-Type"] = "application/json";
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // =========================================================================
    // JSON 便捷方法：自动将 Lua table 序列化为 JSON，自动设置 Content-Type
    // 响应自动解析 JSON 到 result.data 字段
    // =========================================================================

    // shield.http.json(url, data [, options]) -> response_table
    // 通用 JSON POST（最常用场景）
    http.set_function(
        "json",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::object data,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "POST";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            // Serialize Lua table/object to JSON string.
            options.body = lua_table_to_json(data.as<sol::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.json_post(url, data [, options]) -> response_table
    http.set_function(
        "json_post",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::object data,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "POST";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            options.body = lua_table_to_json(data.as<sol::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.json_put(url, data [, options]) -> response_table
    http.set_function(
        "json_put",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::object data,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PUT";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            options.body = lua_table_to_json(data.as<sol::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.json_patch(url, data [, options]) -> response_table
    http.set_function(
        "json_patch",
        [&to_table, &parse_opts](sol::this_state state, std::string url,
                                 sol::object data,
                                 sol::optional<sol::table> opts) -> sol::table {
            sol::state_view lua(state);
            shield::net::HttpClientOptions options;
            options.method = "PATCH";
            options.url = url;
            options.headers["Content-Type"] = "application/json";
            options.headers["Accept"] = "application/json";
            options.body = lua_table_to_json(data.as<sol::table>()).dump();
            if (opts) parse_opts(*opts, options);
            return to_table(lua, shield::net::HttpClient::request(options));
        });

    // shield.http.upload(url, files [, fields] [, timeout]) -> response_table
    // files: array of {field_name, file_path, content_type}
    // fields: table of form field key-value pairs
    http.set_function(
        "upload",
        [&to_table](sol::this_state state, std::string url, sol::table files,
                    sol::optional<sol::table> fields,
                    sol::optional<int> timeout) -> sol::table {
            sol::state_view lua(state);

            std::vector<shield::net::HttpFileField> file_list;
            for (auto& [i, entry] : files) {
                if (entry.is<sol::table>()) {
                    sol::table f = entry.as<sol::table>();
                    shield::net::HttpFileField field;
                    field.field_name =
                        f.get_or<std::string>("field_name", "file");
                    field.file_path = f.get_or<std::string>("file_path", "");
                    field.content_type =
                        f.get_or<std::string>("content_type", "");
                    file_list.push_back(std::move(field));
                }
            }

            std::unordered_map<std::string, std::string> field_map;
            if (fields) {
                for (auto& [k, v] : *fields) {
                    if (k.is<std::string>() && v.is<std::string>()) {
                        field_map[k.as<std::string>()] = v.as<std::string>();
                    }
                }
            }

            auto res = shield::net::HttpClient::upload(
                url, file_list, field_map, timeout.value_or(60));
            return to_table(lua, res);
        });

    // shield.http.download(url, output_path [, timeout]) -> response_table
    http.set_function("download",
                      [&to_table](sol::this_state state, std::string url,
                                  std::string output_path,
                                  sol::optional<int> timeout) -> sol::table {
                          sol::state_view lua(state);
                          auto res = shield::net::HttpClient::download(
                              url, output_path, timeout.value_or(60));
                          return to_table(lua, res);
                      });

    // shield.http.post_form(url, fields [, timeout]) -> response_table
    // fields: table of key-value pairs for application/x-www-form-urlencoded
    http.set_function(
        "post_form",
        [&to_table](sol::this_state state, std::string url, sol::table fields,
                    sol::optional<int> timeout) -> sol::table {
            sol::state_view lua(state);

            std::unordered_map<std::string, std::string> field_map;
            for (auto& [k, v] : fields) {
                if (k.is<std::string>() && v.is<std::string>()) {
                    field_map[k.as<std::string>()] = v.as<std::string>();
                }
            }

            auto res = shield::net::HttpClient::post_form(url, field_map,
                                                          timeout.value_or(10));
            return to_table(lua, res);
        });

    shield["http"] = http;

    // =========================================================================
    // shield.httpd — HTTP 服务端（注册路由处理 incoming 请求）
    // =========================================================================
    auto httpd = lua.create_table();

    // Routes are stored in the runtime's registration table. The bootstrap
    // HTTP bridge (LuaHttpBridge) mirrors them into the HttpServer and
    // dispatches requests back onto the registering service's actor thread.
    auto register_route = [manager, runtime](
                              sol::this_state state, std::string method,
                              std::string path, sol::function handler) {
        if (!manager || !runtime) {
            throw sol::error("shield.httpd is not available in this context");
        }
        const std::string service_id = manager->current_service_id();
        if (service_id.empty()) {
            throw sol::error(
                "shield.httpd routes must be registered from a running "
                "service");
        }
        auto vm = runtime->vm_for_state(state);
        if (!vm) {  // GCOVR_EXCL_LINE (vm_for_state never returns null for a
            throw sol::error(  // registered VM; defensive)
                "registering VM is not managed by the runtime");  // GCOVR_EXCL_LINE
                                                                  // (continuation)
        }
        std::string error;
        if (!runtime->register_http_route(vm, service_id, method, path,
                                          std::move(handler), &error)) {
            throw sol::error("shield.httpd registration failed: " + error);
        }
        sol::state_view lua(state);
        return sol::make_object(lua, true);
    };

    httpd.set_function("get", [register_route](sol::this_state s, std::string p,
                                               sol::function h) {
        return register_route(s, "GET", std::move(p), std::move(h));
    });
    httpd.set_function(
        "post",
        [register_route](sol::this_state s, std::string p, sol::function h) {
            return register_route(s, "POST", std::move(p), std::move(h));
        });
    httpd.set_function("put", [register_route](sol::this_state s, std::string p,
                                               sol::function h) {
        return register_route(s, "PUT", std::move(p), std::move(h));
    });
    httpd.set_function(
        "delete",
        [register_route](sol::this_state s, std::string p, sol::function h) {
            return register_route(s, "DELETE", std::move(p), std::move(h));
        });
    httpd.set_function(
        "patch",
        [register_route](sol::this_state s, std::string p, sol::function h) {
            return register_route(s, "PATCH", std::move(p), std::move(h));
        });

    shield["httpd"] = httpd;
}

void register_plugin_api(sol::table& shield) {
    sol::state_view lua(shield.lua_state());
    auto plugin = lua.create_table();

    // shield.plugin.packages() -> array of {id, version, kind, provides}
    plugin.set_function("packages", [](sol::this_state state) -> sol::table {
        sol::state_view lua(state);
        auto t = lua.create_table();
        for (const auto& p : shield::plugin::global_host().list_packages()) {
            sol::table row = lua.create_table();
            row["id"] = p.id;
            row["version"] = p.version;
            row["kind"] = p.kind;
            row["docs_url"] = p.docs_url;
            row["docs_description"] = p.docs_description;
            sol::table prov = lua.create_table();
            for (size_t i = 0; i < p.provides.size(); ++i)
                prov[i + 1] = p.provides[i];
            row["provides"] = prov;
            t[t.size() + 1] = row;
        }
        return t;
    });

    // shield.plugin.instances() -> array of {id, package, state, required}
    plugin.set_function("instances", [](sol::this_state state) -> sol::table {
        sol::state_view lua(state);
        auto t = lua.create_table();
        for (const auto& in : shield::plugin::global_host().list_instances()) {
            sol::table row = lua.create_table();
            row["id"] = in.id;
            row["package"] = in.package;
            row["state"] = in.state;
            row["required"] = in.required;
            t[t.size() + 1] = row;
        }
        return t;
    });

    // shield.plugin.instance(id) -> table or nil
    plugin.set_function(  // GCOVR_EXCL_LINE (gcov continuation artifact)
        "instance",
        [](sol::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string id)         // GCOVR_EXCL_LINE (lambda entry artifact)
        -> sol::object {           // GCOVR_EXCL_LINE (lambda entry artifact)
            sol::state_view lua(state);
            for (const auto& in :
                 shield::plugin::global_host().list_instances()) {
                if (in.id == id) {
                    sol::table row = lua.create_table();
                    row["id"] = in.id;
                    row["package"] = in.package;
                    row["state"] = in.state;
                    row["required"] = in.required;
                    return row;
                }
            }
            return sol::nil;
        });

    // shield.plugin.binding(name) -> {instance_id, interface} or nil
    plugin.set_function(  // GCOVR_EXCL_LINE (gcov continuation artifact)
        "binding",
        [](sol::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string name)       // GCOVR_EXCL_LINE (lambda entry artifact)
        -> sol::object {           // GCOVR_EXCL_LINE (lambda entry artifact)
            sol::state_view lua(state);
            auto b = shield::plugin::global_host().get_binding(name);
            if (!b) return sol::nil;
            sol::table row = lua.create_table();
            row["instance_id"] = b->instance_id;
            row["interface"] = b->interface_name;
            return row;
        });

    shield["plugin"] = plugin;
}

void register_full_shield_api(sol::state& lua, LuaServiceManager* manager,
                              LuaRuntime* runtime) {
    // Initialize HTTP client (libcurl global state).
    shield::net::HttpClient::initialize();

    // Register usertypes
    ServiceHandle::register_usertype(lua);
    register_client_identity_api(lua);

    auto shield = lua.create_table();

    register_service_api(shield, manager);
    register_message_api(shield, manager, runtime);
    register_timer_api(shield, manager, runtime);
    register_task_api(shield, manager, runtime);
    register_config_api(shield);
    register_log_api(shield, manager);
    register_client_api(shield, manager);
    register_http_api(shield, manager, runtime);
    register_plugin_api(shield);

#ifdef SHIELD_ENABLE_CLUSTER
    register_cluster_api(shield, manager);
#endif

#ifdef SHIELD_ENABLE_PLAYER
    register_player_api(shield, manager);
#else
    register_player_stub_api(shield, lua);
#endif

#ifdef SHIELD_ENABLE_SERVER
    register_server_api(shield, manager, runtime);
#else
    register_server_stub_api(shield, lua);
#endif

#ifdef SHIELD_ENABLE_GLOBAL
    register_global_api(shield, manager, runtime);
#else
    register_global_stub_api(shield, lua);
#endif

    lua["shield"] = shield;

    // Client RPC namespace (architecture-correction M1). Populated by the
    // gateway actor integration: shield.client.bind/close (M2) and one
    // helper per s2c descriptor (M2). Existing as an empty table now keeps
    // the API surface stable for scripts written against the new contract.
    lua["shield"]["client_rpc"] = lua.create_table();

    // AD-07: hook os.time / os.date so Lua business code also reads the
    // business-time clock (adjustable in tests). Only the no-arg forms are
    // redirected; os.time(table) and os.date(fmt, t) keep their original
    // semantics (pure conversion). os.clock() is NOT touched — it is used
    // for real CPU-time measurement (e.g. busy-wait loops).
    {
        sol::table os_t = lua["os"];
        sol::function orig_time = os_t["time"];
        sol::function orig_date = os_t["date"];
        // os.time(): no-arg → business clock seconds; with table → original.
        os_t.set_function("time",
                          [manager, orig_time = std::move(orig_time)](
                              sol::optional<sol::table> t) -> sol::object {
                              sol::state_view lua(orig_time.lua_state());
                              if (t.has_value()) {
                                  return orig_time(t.value());
                              }
                              return sol::make_object(
                                  lua, manager->clock_now_seconds());
                          });
        // os.date(fmt): no time → business clock; with time → original.
        os_t.set_function(
            "date",
            [manager, orig_date = std::move(orig_date)](
                sol::optional<std::string> fmt,
                sol::optional<double> t) -> sol::object {
                double when =
                    t.has_value()
                        ? t.value()
                        : static_cast<double>(manager->clock_now_seconds());
                return orig_date(fmt, when);
            });
    }

    // Coroutine dispatch helper used by
    // GCOVR_EXCL_START (gcov clone artifact; attribution drift under reformat)
    // LuaRuntime::call_service_method_coroutine. It wraps a handler + args
    // table into a coroutine whose body returns the handler's results, so a
    // yield inside the handler (e.g. shield.sleep) suspends the whole coroutine
    // and a later resume continues transparently. Defined as a global function
    // directly so a script error can't take down register_api
    // (call_service_method_coroutine falls back to sync dispatch if the helper
    // is absent).
    lua.safe_script(
        "function __shield_run_handler(handler, args)\n"
        // GCOVR_EXCL_STOP
        "  return coroutine.create(function()\n"
        "    return handler(table.unpack(args, 1, args.n or #args))\n"
        "  end)\n"
        "end",          // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           sol::protected_function_result  // GCOVR_EXCL_LINE (gcov clone
                                           // artifact)
               pfr)                        // GCOVR_EXCL_LINE (gcov
                                           // clone artifact)
        -> sol::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
}

}  // namespace shield::lua
