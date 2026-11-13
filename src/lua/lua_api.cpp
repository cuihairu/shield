// [SHIELD_LUA] Lua API registration: shared converters, service/message/
// timer/task/config/log, client identity and cluster bindings, plus the
// register_full_shield_api entry point.
#include "shield/lua/lua_api.hpp"

#include "lua_api_detail.hpp"
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
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shield/core/service_message.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/binding.hpp"
#include "shield/lua/client_identity.hpp"
#include "shield/lua/lua_constants.hpp"
#include "shield/lua/lua_crypto.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/lua/player_ref_box.hpp"
#include "shield/net/http_client.hpp"
#include "shield/net/session.hpp"
#include "shield/plugin/plugin_host.hpp"

// [SHIELD_LUA] Lua API registration
#include "shield/config/config.hpp"
#include "shield/lua/lua_api.hpp"
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
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "shield/core/service_message.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/binding.hpp"
#include "shield/lua/client_identity.hpp"
#include "shield/lua/lua_constants.hpp"
#include "shield/lua/lua_crypto.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/lua/player_ref_box.hpp"
#include "shield/net/http_client.hpp"
#include "shield/net/session.hpp"
#include "shield/plugin/plugin_host.hpp"

namespace shield::lua {

// ServiceHandle userdata constructor for the registration-time lambdas.
namespace {
shd::object make_service_handle(shd::state_view lua, ServiceHandle handle) {
    return shd::make_userdata<ServiceHandle>(lua, "ServiceHandle",
                                             std::move(handle));
}
}  // namespace

shd::table make_error(shd::this_state state, std::string code,
                      std::string message, bool retryable, shd::object detail) {
    shd::state_view lua(state);
    shd::table err = lua.create_table();
    err["code"] = std::move(code);
    err["message"] = std::move(message);
    err["retryable"] = retryable;
    if (detail.valid() &&      // GCOVR_EXCL_BR_LINE (defensive: no detail)
        detail != shd::nil) {  // GCOVR_EXCL_BR_LINE (defensive: no
                               // coverage-suite caller passes a detail object)
        err["detail"] = detail;  // GCOVR_EXCL_LINE (no coverage-suite caller
                                 // passes a detail object)
    }
    return err;
}  // GCOVR_EXCL_LINE (gcov epilogue artifact: block of the trailing brace never
   // accounts)

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
// Lua cannot construct them (shd::no_constructor) and cannot mutate them.
// Passing either as a message argument serializes back to the marker form.

namespace {

bool extract_client_data(const shd::object& object, ClientContextData* out) {
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

/// Releases the registry reference that anchors a sleeping coroutine exactly
/// once, whichever of the mutually exclusive outcomes happens:
///  * the timer fires and the continuation runs — the continuation calls
///    unref() on its terminal / re-yield arms;
///  * the timer is cancelled or the service exits before firing — the stored
///    callback is destroyed without running, and the destructor unrefs;
///  * scheduling fails before the callback is stored — the caller's local
///    copy drops at the end of _resume_after and the destructor unrefs.
/// Without this guard the registry reference (and with it the suspended
/// coroutine thread and everything it holds) leaked on the last two paths.
/// The exit / shutdown paths destroy these callbacks before the owning
/// lua_State is closed, so the destructor's luaL_unref runs on a live state.
class SleepAnchor {
public:
    SleepAnchor(lua_State* co, int ref, std::shared_ptr<LuaVM> vm)
        : co_(co), ref_(ref), vm_(std::move(vm)) {}
    SleepAnchor(const SleepAnchor&) = delete;
    SleepAnchor& operator=(const SleepAnchor&) = delete;
    ~SleepAnchor() {
        if (!released_) {
            luaL_unref(co_, LUA_REGISTRYINDEX, ref_);
        }
    }
    /// Release the reference (called by the running continuation). Idempotent.
    void unref() {
        if (!released_) {  // GCOVR_EXCL_BR_LINE (defensive: unref is
                           // idempotent; the double-release arm is a contract
                           // guard)
            released_ = true;
            luaL_unref(co_, LUA_REGISTRYINDEX, ref_);
        }
    }

private:
    lua_State* co_;
    int ref_;
    bool released_ = false;

    // Declared last so it is destroyed after luaL_unref() in ~SleepAnchor.
    std::shared_ptr<LuaVM> vm_;
};

}  // namespace

// Reads the client identity out of a ClientContext/ClientRef argument.
// Used by shield.client.bind/close and the client_rpc egress helpers.
//
// SEC-3: only the read-only identity userdata (ClientContextBox / ClientRefBox,
// which Lua cannot construct or mutate) is accepted. A bare
// __shield_client_ref marker table is NOT trusted here: the marker is the wire
// form that only the gateway produces, and by the time a service handler sees
// it (via json_to_lua) it is already materialized as the userdata. Accepting
// the raw table would let an untrusted message payload passed straight into
// shield.client.bind/close/_client_egress forge a session identity.
static bool client_arg_to_data(const shd::object& object,
                               ClientContextData* out) {
    if (extract_client_data(object, out)) {
        return true;
    }
    return false;
}

// Canonical JSON -> Lua converter. Marker objects materialize as read-only
// identity userdata (see the __shield_*_ref handling below).
shd::object json_to_lua(shd::state_view lua, const nlohmann::json& value) {
    if (value.is_null()) {
        return shd::make_object(lua, shd::nil);
    }
    if (value.is_boolean()) {
        return shd::make_object(lua, value.get<bool>());
    }
    if (value.is_number_integer()) {
        return shd::make_object(lua, value.get<std::int64_t>());
    }
    // GCOVR_EXCL_START (unreachable: is_number_integer() also matches
    // unsigned values, so this arm can never be selected)
    if (value.is_number_unsigned()) {
        return shd::make_object(lua, value.get<std::uint64_t>());
    }
    // GCOVR_EXCL_STOP
    if (value.is_number_float()) {
        return shd::make_object(lua, value.get<double>());
    }
    if (value.is_string()) {
        return shd::make_object(lua, value.get<std::string>());
    }
    if (value.is_array()) {
        shd::table table = lua.create_table();
        int index = 1;
        for (const auto& item : value) {
            table[index++] = json_to_lua(lua, item);
        }
        return shd::make_object(lua, table);
    }
    if (value.is_object()) {
#ifdef SHIELD_ENABLE_PLAYER
        // A __shield_player_ref marker materializes as the read-only
        // PlayerRef userdata (value semantics, no mailbox). Missing fields
        // degrade to defaults, mirroring the client-identity handling.
        if (value.contains("__shield_player_ref") &&
            value.value("__shield_player_ref", false) ==
                true) {  // GCOVR_EXCL_BR_LINE (compiler artifact: nlohmann
                         // value() key-absence clone; the contains() guard
                         // makes it dead)
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
            return shd::make_object(lua, PlayerRefBox{std::move(ref)});
        }
#endif
        // A trusted client-identity marker materializes as the read-only
        // ClientContext userdata. from_json does all field validation, so a
        // malformed field degrades to its default instead of throwing.
        if (auto ctx = ClientContextData::from_json(value)) {
            shd::object maybe_ud = lua["__shield_make_client_context"];
            if (maybe_ud.valid() && maybe_ud.is<shd::protected_function>()) {
                shd::protected_function make_context =
                    maybe_ud.as<shd::protected_function>();
                auto result = make_context(ctx->session_id, ctx->session_epoch,
                                           ctx->player_id, ctx->gateway_address,
                                           ctx->protocol_profile_id);
                if (result.valid() && result.return_count() > 0) {
                    return result.get<shd::object>(0);
                }
            }
        }
        shd::table table = lua.create_table();
        for (const auto& [key, item] : value.items()) {
            table[key] = json_to_lua(lua, item);
        }
        return shd::make_object(lua, table);
    }
    return shd::make_object(lua, shd::nil);
}

// Serialize client identity userdata back to its __shield_client_ref marker
// form (the inverse of the json_to_lua materialization). Both boxes wrap the
// same trusted snapshot, so either shape serializes identically.
nlohmann::json box_context_marker(const shd::object& value) {
    if (value.is<ClientContextBox>()) {
        return value.as<const ClientContextBox&>().data.to_json();
    }
    return value.as<const ClientRefBox&>().data.to_json();
}

#ifdef SHIELD_ENABLE_PLAYER
// PlayerRef userdata travels in its marker form (the inverse of the
// json_to_lua materialization).
nlohmann::json box_player_marker(const shd::object& value) {
    const auto& d = value.as<const PlayerRefBox&>().data;
    return nlohmann::json{{"__shield_player_ref", true},
                          {"uid", d.uid},
                          {"node_id", d.node_id},
                          {"service_id", d.service_id},
                          {"epoch", d.epoch}};
}
#endif  // SHIELD_ENABLE_PLAYER

nlohmann::json lua_table_to_json(const shd::table& table) {
    bool array_like = true;
    std::size_t max_index = 0;
    std::size_t entry_count = 0;

    for (const auto& [key, _] : table) {
        ++entry_count;
        shd::object key_obj = key;
        // Read the key at full 64-bit width: is<int>() only checks the
        // integer type, and as<int>() would silently narrow a key like
        // 2^32+1 down to 1 — making the single-key table look array-like,
        // taking the array branch, and reading a table[1] that does not
        // exist (the value was lost). Anything outside the positive int
        // range is an object key, same as the runtime converter.
        if (!key_obj.is<std::int64_t>()) {
            array_like = false;
            break;
        }

        const std::int64_t index = key_obj.as<std::int64_t>();
        if (index <= 0 || index > std::numeric_limits<int>::max()) {
            array_like = false;
            break;
        }
        max_index = std::max(max_index, static_cast<std::size_t>(index));
    }

    if (array_like &&
        max_index == entry_count) {  // GCOVR_EXCL_BR_LINE (integration: suites
                                     // never build a sparse array-like table)
        nlohmann::json array =
            nlohmann::json::array();  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // nlohmann construction arcs)
        for (std::size_t i = 1; i <= max_index; ++i) {
            array.push_back(
                lua_to_json(shd::object(table[static_cast<int>(i)])));
        }
        return array;  // GCOVR_EXCL_BR_LINE (compiler artifact: return arc)
    }  // GCOVR_EXCL_LINE

    nlohmann::json object = nlohmann::json::object();
    for (const auto& [key, value] : table) {
        shd::object key_obj = key;
        std::string object_key;
        if (key_obj.is<std::string>()) {
            object_key = key_obj.as<std::string>();
        } else if (key_obj.is<std::int64_t>()) {
            // 64-bit integer keys: a Lua integer above INT_MAX must survive
            // (audit P2); is<int>()/as<int>() would silently drop it.
            object_key = std::to_string(key_obj.as<std::int64_t>());
        } else {
            continue;
        }
        object[object_key] = lua_to_json(value);
    }
    return object;
}

nlohmann::json variadic_to_json_array(shd::variadic_args args) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& arg : args) {
        values.push_back(lua_to_json(shd::object(arg)));
    }
    return values;
}  // GCOVR_EXCL_LINE

// Helper to extract service ID from ServiceHandle or string
std::string extract_service_id(const shd::object& target) {
    std::string id;
    if (target.is<ServiceHandle>()) {
        id = target.as<ServiceHandle>().id();
    } else if (target.is<std::string>()) {
        id = target.as<std::string>();
    }
    return id;
}  // GCOVR_EXCL_LINE (function-exit arc artifact of extract_service_id)

void register_service_api(shd::table& shield, LuaServiceManager* manager) {
    // Synchronous spawn primitive: runs VM creation + on_init on the calling
    // thread. The public shield.spawn wrapper (below) uses this on the main
    // thread, inside a spawn's on_init, and as the fallback when the
    // coroutine path cannot suspend.
    shield.set_function(
        "_sync_spawn",
        [manager](shd::this_state state, std::string module,
                  std::optional<shd::table> opts) -> shd::variadic_results {
            shd::state_view lua(state);
            shd::variadic_results results;

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
                results.push_back(shd::make_object(lua, shd::nil));
                results.push_back(
                    make_error(state, std::move(code), result.error_message));
                return results;
            }

            // Return ServiceHandle userdata instead of string
            ServiceHandle handle(result.service_id);
            results.push_back(make_service_handle(lua, handle));
            results.push_back(shd::make_object(lua, shd::nil));
            return results;
        });

    shield.set_function("exit", [manager](std::optional<std::string> reason) {
        manager->request_current_exit(reason.value_or("normal"));
    });

    // True while a spawn's on_init runs its initial segment on the spawning
    // thread: shield.spawn resolves synchronously there (the child's init
    // blocks the spawner, never a service actor).
    shield.set_function("_in_on_init", [] {
        return LuaServiceManager::spawn_init_in_progress();
    });

    shield.set_function(
        "self", [manager](shd::this_state state) -> shd::object {
            shd::state_view lua(state);
            const auto service_id = manager->current_service_id();
            if (service_id.empty()) {
                return shd::make_object(lua, shd::nil);
            }
            ServiceHandle handle(service_id);
            return make_service_handle(lua, handle);
        });

    shield.set_function("names",
                        [manager](shd::this_state state) -> shd::table {
                            shd::state_view lua(state);
                            shd::table names = lua.create_table();

                            int index = 1;
                            for (const auto& name : manager->list_services()) {
                                names[index++] = name;
                            }
                            return names;
                        });  // GCOVR_EXCL_LINE (function-exit arc artifact of
                             // the names lambda)

    shield.set_function(
        "query",
        [manager](shd::this_state state,
                  std::string name) -> shd::variadic_results {
            shd::state_view lua(state);
            shd::variadic_results results;

            const auto service = manager->query_service(name);
            if (!service.empty()) {
                ServiceHandle handle(service);
                results.push_back(make_service_handle(lua, handle));
                results.push_back(shd::make_object(lua, shd::nil));
                return results;
            }

            results.push_back(shd::make_object(lua, shd::nil));
            results.push_back(make_error(state, "service_not_found",
                                         "service not found: " + name));
            return results;
        });

    shield.set_function(
        "register",
        [manager](shd::this_state state,
                  std::string name) -> shd::variadic_results {
            shd::state_view lua(state);
            shd::variadic_results results;

            std::string error;
            if (!manager->register_name(name, &error)) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error(state, "register_failed", error));
                return results;
            }

            results.push_back(shd::make_object(lua, true));
            results.push_back(shd::make_object(lua, shd::nil));
            return results;
        });

    shield.set_function("unregister",
                        [manager](shd::this_state state,
                                  std::string name) -> shd::variadic_results {
                            shd::state_view lua(state);
                            shd::variadic_results results;

                            std::string error;
                            if (!manager->unregister_name(name, &error)) {
                                results.push_back(shd::make_object(lua, false));
                                results.push_back(make_error(
                                    state, "unregister_failed", error));
                                return results;
                            }

                            results.push_back(shd::make_object(lua, true));
                            results.push_back(shd::make_object(lua, shd::nil));
                            return results;
                        });

    // Blue-green handover: the replacement service atomically takes over an
    // existing name from its current owner (which then drains and exits).
    // Same dispatch-context rule as register/unregister: the claiming
    // service calls this in its own handler.
    shield.set_function(
        "claim",
        [manager](shd::this_state state,
                  std::string name) -> shd::variadic_results {
            shd::state_view lua(state);
            shd::variadic_results results;

            std::string error;
            if (!manager->claim_name(name, &error)) {
                results.push_back(shd::make_object(lua, false));
                results.push_back(make_error(state, "claim_failed", error));
                return results;
            }

            results.push_back(shd::make_object(lua, true));
            results.push_back(shd::make_object(lua, shd::nil));
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
        [manager](shd::this_state state, std::string module,
                  std::optional<shd::table> opts, int timeout_ms) -> uint64_t {
            if (manager->current_service_id().empty()) {
                return 0;
            }
            nlohmann::json options =
                opts ? lua_table_to_json(*opts) : nlohmann::json::object();
            if (!options.is_object()) {
                options = nlohmann::json::object();
            }

            lua_State* co = state;
            const uint64_t session =
                manager->suspend_for_call(co, timeout_ms, module);
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
        });  // GCOVR_EXCL_BR_LINE (compiler artifact: shd argument-conversion
             // template arcs at the set_function boundary)

    // Rebuild a ServiceHandle userdata from a service id. Used by the
    // shield.spawn wrapper after a coroutine resume (the response channel
    // carries JSON, not userdata).
    shield.set_function(
        "_make_handle",  // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](shd::this_state state,  // GCOVR_EXCL_LINE
           std::string service_id) -> shd::object {
            shd::state_view lua(state);
            ServiceHandle handle(std::move(service_id));
            return make_service_handle(lua, handle);
        });

    // Business-triggered panic: invoke on_panic(reason, {type="explicit"})
    // and exit the current service with reason "panic".
    shield.set_function("panic", [manager](std::optional<std::string> reason) {
        manager->panic_current(reason.value_or("explicit panic"));
    });

    // Public shield.spawn: suspend inside handler coroutines (the spawn
    // worker runs on_init off-actor), stay synchronous on the main thread and
    // inside a spawn's on_init (on the spawning thread).
    shd::state_view lua(shield.lua_state());
    lua["shield"] = shield;
    shd::safe_script(
        lua,
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
           shd::protected_function_result  // GCOVR_EXCL_LINE (gcov clone
                                           // artifact)
               pfr)                        // GCOVR_EXCL_LINE (gcov
                                           // clone artifact)
        -> shd::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
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

void register_message_api(shd::table& shield, LuaServiceManager* manager,
                          LuaRuntime* runtime) {
    shield.set_function(
        "send",
        [manager](shd::this_state state, shd::object target, std::string method,
                  shd::variadic_args args) -> shd::variadic_results {
            shd::state_view lua(state);
            shd::variadic_results results;

            std::string target_id = extract_service_id(target);
            if (target_id.empty()) {
                results.push_back(shd::make_object(lua, false));
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
                    results.push_back(shd::make_object(lua, false));
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
                    results.push_back(shd::make_object(lua, true));
                    results.push_back(shd::make_object(lua, shd::nil));
                    return results;
                }
                results.push_back(shd::make_object(lua, false));
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
                results.push_back(shd::make_object(lua, false));
                results.push_back(
                    make_error(state, std::move(code), error, retryable));
                return results;
            }

            results.push_back(shd::make_object(lua, true));
            results.push_back(shd::make_object(lua, shd::nil));
            return results;
        });

    // Coroutine-aware call primitive. Suspends the caller's coroutine and
    // sends a call-request message to the target; the caller is resumed with
    // [ok, values...] when the callee completes (or on timeout). Returns the
    // session id (0 if the caller is not inside a coroutine).
    shield.set_function(
        "_coro_call",
        [manager](shd::this_state state, shd::object target, std::string method,
                  shd::table args, int timeout_ms) -> uint64_t {
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
                                              timeout_ms, target_id);
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
            shd::object packed_count = args["n"];
            if (packed_count.valid() && packed_count.is<int>()) {
                const int n = packed_count.as<int>();
                arg_count = n > 0 ? static_cast<std::size_t>(n) : 0;
            }
            nlohmann::json json_args = nlohmann::json::array();
            for (std::size_t i = 1; i <= arg_count; ++i) {
                json_args.push_back(
                    lua_to_json(shd::object(args[static_cast<int>(i)])));
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
                    manager->suspend_for_call(co, timeout_ms, target_id);
                if (!remote.error_code.empty()) {
                    manager->complete_call(
                        session, false,
                        nlohmann::json::array(  // GCOVR_EXCL_BR_LINE (compiler
                                                // artifact: init-list cold
                                                // clones)
                            {nlohmann::json::object(  // GCOVR_EXCL_BR_LINE
                                                      // (compiler artifact:
                                                      // nlohmann init-list)
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
                        nlohmann::json::array(  // GCOVR_EXCL_BR_LINE (compiler
                                                // artifact: init-list cold
                                                // clones)
                            {nlohmann::json::object(  // GCOVR_EXCL_BR_LINE
                                                      // (compiler artifact:
                                                      // nlohmann init-list)
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
                    manager->suspend_for_call(co, timeout_ms, target_id);
                manager->complete_call(
                    session, false,
                    nlohmann::json::array(  // GCOVR_EXCL_BR_LINE (compiler
                                            // artifact: nlohmann init-list
                                            // construction arcs)
                        {nlohmann::json::object(  // GCOVR_EXCL_BR_LINE
                                                  // (compiler artifact:
                                                  // nlohmann init-list
                                                  // construction arcs)
                            {{"code", "service_not_found"},
                             {"message", "service not found: " + target_id},
                             {"retryable", false}})}));
                return session;
            }
            lua_State* co = state;
            const uint64_t session =
                manager->suspend_for_call(co, timeout_ms, target_id);

            // Build and queue the call-request message.
            std::string send_error;
            // Send carries call_session so the callee's dispatch can route the
            // response back to the caller.
            // Branch-only exclusion: the failure arm (guarded by the
            // START/STOP block below) is the target-actor-died race window,
            // which the deterministic suites cannot hit; the branch on this
            // line belongs to the same unreachable arm.
            if (!manager->send_call_request(  // GCOVR_EXCL_BR_LINE (defensive:
                                              // unreachable race window, see
                                              // START/STOP below)
                    service_id,
                    method,  // GCOVR_EXCL_BR_LINE (defensive: unreachable race
                             // window, see START/STOP below)
                    json_args, session,
                    &send_error)) {  // GCOVR_EXCL_BR_LINE (defensive:
                                     // unreachable race window)
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
        });  // GCOVR_EXCL_BR_LINE (compiler artifact: shd argument-conversion
             // template arcs at the set_function boundary)

    shield.set_function("_is_in_exit",
                        [manager]() -> bool { return manager->is_in_exit(); });

    // Target-shape check for the call wrappers: returns the normalized
    // service id, or nil when the target is neither a ServiceHandle nor a
    // string. Shape errors are programming errors and are reported with
    // invalid_target regardless of the calling context.
    shield.set_function(
        "_call_target_id",
        [](shd::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           shd::object target) -> std::optional<std::string> {
            const std::string id = extract_service_id(target);
            if (id.empty()) {
                return shd::nullopt;
            }
            return id;
        });

    // Stable error code for a raw call-failure message (used by the call
    // wrapper to shape non-table resume payloads into {code, message}).
    shield.set_function("_call_error_code",  // GCOVR_EXCL_LINE
                        [](std::optional<    // GCOVR_EXCL_LINE
                            std::string>
                               msg) -> std::string {
                            return call_error_code_for(msg.value_or(""));
                        });

    // DEPRECATED: Use ctx.sender instead. Kept for backward compatibility.
    // In new code, prefer: function M.handler(ctx, ...) local src = ctx.sender
    // end
    shield.set_function("sender", [manager]() -> std::optional<std::string> {
        // Returns nil in timer/fork context (no sender).
        // Returns nil outside any dispatch (module-level code).
        // The distinction between "no sender" and "context_expired" is
        // that in timer/fork context we ARE inside a dispatch scope but
        // the sender is empty; outside any scope the context is expired.
        const auto sender = manager->current_sender_id();
        if (sender.empty()) {
            return shd::nullopt;
        }
        return sender;
    });

    // DEPRECATED: Use ctx.trace instead. Kept for backward compatibility.
    shield.set_function("trace", [manager]() -> std::optional<std::string> {
        const auto trace = manager->current_trace_id();
        if (trace.empty())  // GCOVR_EXCL_LINE (no active trace in coverage
            return shd::nullopt;  // suites reach this shim)
        return trace;             // GCOVR_EXCL_LINE (continuation)
    });

    // DEPRECATED: Use ctx.deadline instead. Kept for backward compatibility.
    shield.set_function("deadline", [manager]() -> std::optional<int64_t> {
        const auto dl = manager->current_deadline_ms();
        if (dl <= 0)  // GCOVR_EXCL_LINE (no active deadline in coverage
            return shd::nullopt;  // suites reach this shim)
        return dl;                // GCOVR_EXCL_LINE (continuation)
    });

    // shield.call / shield.call_timeout are coroutine-only: inside a handler
    // coroutine (handler, timer/fork callback, on_init) the caller suspends
    // until the callee completes; on the main thread — module-level code —
    // they are rejected with a stable error code instead of blocking the
    // worker.
    shd::state_view lua(shield.lua_state());
    lua["shield"] = shield;
    shd::safe_script(
        lua,
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
           shd::protected_function_result  // GCOVR_EXCL_LINE (gcov clone
                                           // artifact)
               pfr)                        // GCOVR_EXCL_LINE (gcov
                                           // clone artifact)
        -> shd::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
}

void register_timer_api(shd::table& shield, LuaServiceManager* manager,
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
                  shd::function callback) -> shd::variadic_results {
            shd::variadic_results results;
            shd::state_view lua(callback.lua_state());

            // Get current service ID
            const std::string service_id = manager->current_service_id();

            // Check timer limit.
            const auto timer_count = manager->active_actor_timer_count();
            if (timer_count >= kTimerLimit) {
                results.push_back(shd::make_object(lua, shd::nil));
                shd::this_state ts(callback.lua_state());
                results.push_back(
                    make_error(ts, "timer_limit", "timer limit reached"));
                return results;
            }

            // GCOVR_EXCL_BR_START (compiler artifact: the outlined cold clone
            // of this ternary carries branch records of its own that never
            // run; the live arms are both driven by the timer suites. The
            // region form survives the clone's line attribution moving
            // between the ?-arm and the :-arm across rebuilds)
            const uint64_t id = service_id.empty()
                                    ? 0
                                    : manager->schedule_actor_timer_once(
                                          delay_ms, callback, service_id);
            // GCOVR_EXCL_BR_STOP
            results.push_back(shd::make_object(lua, id));
            return results;
        });

    shield.set_function(
        "timer",
        [manager](int interval_ms,
                  shd::function callback) -> shd::variadic_results {
            shd::variadic_results results;
            shd::state_view lua(callback.lua_state());

            // Get current service ID
            const std::string service_id = manager->current_service_id();

            // Check timer limit.
            const auto timer_count = manager->active_actor_timer_count();
            if (timer_count >= kTimerLimit) {
                results.push_back(shd::make_object(lua, shd::nil));
                shd::this_state ts(callback.lua_state());
                results.push_back(
                    make_error(ts, "timer_limit", "timer limit reached"));
                return results;
            }

            // GCOVR_EXCL_BR_START (compiler artifact: the outlined cold clone
            // of this ternary carries branch records of its own that never
            // run; the live arms are both driven by the timer suites. The
            // region form survives the clone's line attribution moving
            // between the ?-arm and the :-arm across rebuilds)
            const uint64_t id = service_id.empty()
                                    ? 0
                                    : manager->schedule_actor_timer_fixed_delay(
                                          interval_ms, callback, service_id);
            // GCOVR_EXCL_BR_STOP
            results.push_back(shd::make_object(lua, id));
            return results;
        });

    shield.set_function(
        "cancel_timer",
        [manager](shd::this_state state, uint64_t id) -> shd::variadic_results {
            shd::state_view lua(state);
            shd::variadic_results results;

            const bool cancelled = manager->cancel_actor_timer(id);
            results.push_back(shd::make_object(lua, cancelled));
            if (!cancelled) {
                results.push_back(
                    make_error(state, "timer_not_found",
                               "Timer not found or already completed"));
            } else {
                results.push_back(shd::make_object(lua, shd::nil));
            }
            return results;
        });  // GCOVR_EXCL_LINE (function-exit arc artifact of the
             // cancel_timer lambda)

    // shield.sleep is implemented as a Lua wrapper that schedules a native
    // timer to resume the current coroutine and then yields. The C primitive
    // _resume_after anchors the running coroutine against GC and arms the
    // timer; coroutine.yield suspends until the timer fires and resumes us.
    shield.set_function("_resume_after", [manager, runtime](
                                             shd::this_state state,
                                             int delay_ms) {
        if (delay_ms < 0) {
            delay_ms = 0;
        }
        lua_State* co = state;  // current coroutine thread
        // Anchor the thread so it survives GC while suspended. The guard
        // releases the reference exactly once whether the timer fires (the
        // continuation unrefs below), is cancelled / the service exits before
        // firing (the stored callback is dropped without running and the
        // destructor unrefs), or scheduling fails before the callback is
        // stored (the local copy drops at the end of this lambda).
        lua_pushthread(co);
        const int ref = luaL_ref(co, LUA_REGISTRYINDEX);
        const std::string service_id = manager->current_service_id();
        auto anchor = std::make_shared<SleepAnchor>(
            co, ref,
            // GCOVR_EXCL_BR_START (defensive: the sleep schedule site is only
            // reached from a running coroutine, which always has a VM)
            runtime ? runtime->vm_for_state(co) : nullptr);
        // GCOVR_EXCL_BR_STOP
        auto resume_fn = [co, anchor, manager, service_id]() {
            // Keep the service VM alive for the whole continuation: a
            // terminal error can trip the consecutive-error panic
            // threshold, whose exit() erases the service from the registry
            // — without this handle the VM's last reference dies with
            // invoke_error_hook's frame and lua_close runs before the tail
            // luaL_unref below (SIGSEGV in lua_rawgeti, observed by
            // SleepContinuationErrorsCountTowardPanic when the 10th sleep
            // error panics the service).
            std::shared_ptr<LuaVM> vm_keepalive =
                manager->service_vm(service_id);
            // Another resume source already completed this coroutine (e.g.
            // the service exited, or a call completion resumed it while the
            // sleep timer was still armed): nothing to resume and no error to
            // report. Without this guard the lua_resume below would report a
            // bogus "sleep continuation error" against a healthy service.
            // GCOVR_EXCL_START (defensive: the not-yielded arm is a
            // resume-source race — the service exited or a call completion
            // resumed the coroutine while the sleep timer was still armed;
            // no test can force that interleaving deterministically)
            if (lua_status(co) != LUA_YIELD) {
                anchor->unref();
                return;
            }
            // GCOVR_EXCL_STOP
            int nres = 0;
            // Driving-phase registration: a call completion arriving
            // while this guard is held is re-enqueued by resume_caller
            // instead of racing this resume.
            std::unique_ptr<LuaServiceManager::DrivingGuard> driving =
                std::make_unique<LuaServiceManager::DrivingGuard>(
                    *manager, co, "sleep-timer");
            const int status = lua_resume(co, nullptr, 0, &nres);
            if (status == LUA_YIELD) {
                // Yielded again (e.g. another sleep/call): the API
                // that yielded has already anchored the coroutine for
                // its own resume source, so release this sleep anchor.
                anchor->unref();
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
                    shd::stack_object so(shd::state_view(co), i + 1);
                    returns.push_back(lua_to_json(so));
                }
                manager->on_handler_completed(co, returns);
            } else {
                // Terminal error: mirror invoke_coroutine's error arm —
                // drain the error object off the coroutine stack, route
                // an in-flight call response upstream, and count the
                // error against the service's on_error hook / panic
                // threshold.
                std::string msg = "sleep continuation error";
                if (lua_type(co, -1) == LUA_TSTRING) {
                    msg = lua_tostring(co, -1);
                }
                lua_settop(co, 0);
                // Branch-only exclusions: resume_fn is only ever scheduled
                // when service_id is non-empty (the schedule site guards on
                // it), so the empty arms below are unreachable.
                if (!service_id.empty()) {  // GCOVR_EXCL_BR_LINE (defensive:
                                            // resume_fn only runs with a
                                            // non-empty service_id)
                    manager->on_handler_failed(co, msg);
                    manager->invoke_error_hook(service_id, "sleep", "", msg);
                }
            }
            // Terminal segment (LUA_OK or error): honor a shield.exit
            // the continuation requested. During spawn-init the spawn
            // path owns the request instead.
            // Branch-only exclusion: the same scheduling precondition makes
            // the empty-service arm unreachable, and spawn_init_in_progress()
            // is a spawn-thread local that is always false on the timer
            // thread that runs this continuation.
            if (!service_id.empty() &&  // GCOVR_EXCL_BR_LINE (defensive: see
                                        // note above)
                !LuaServiceManager::
                    spawn_init_in_progress()) {  // GCOVR_EXCL_BR_LINE
                                                 // (defensive: thread-local
                                                 // never set on the timer
                                                 // thread)
                manager->finish_pending_exit(service_id);
            }
            // LUA_OK (completed) or an error: release the anchor.
            anchor->unref();
        };  // GCOVR_EXCL_BR_LINE (compiler artifact: the inlined resume_fn
            // lambda body produces a pseudo-branch on the closing brace)
        if (!service_id.empty()) {  // GCOVR_EXCL_BR_LINE (defensive: the empty
                                    // service_id arm is unreachable — this
                                    // schedule site is only reached from a
                                    // running coroutine, which always has a
                                    // current service)
            (void)manager->schedule_actor_timer_once_fn(
                delay_ms, std::move(resume_fn), service_id);
        }
        // If service_id was empty (unreachable in practice — the schedule
        // site is only reached from a running coroutine) or scheduling failed
        // (the service left the registry / the driver actor could not spawn,
        // both returning 0), resume_fn was never stored and will never run;
        // the local anchor copy drops here and its destructor unrefs.
    });

    // Blocking sleep used when shield.sleep is invoked outside any coroutine
    // (e.g. from module-level code). The coroutine-aware branch below handles
    // the yieldable case inside service handlers.
    shield.set_function("_block_sleep", [](int delay_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    });

    shd::state_view lua(shield.lua_state());
    lua["shield"] = shield;
    // Define shield.sleep in Lua so it can use coroutine.yield natively. When
    // not running inside a coroutine, block the calling thread so the call
    // still completes.
    shd::safe_script(
        lua,
        "shield.sleep = function(ms)\n"
        "  local _, ismain = coroutine.running()\n"
        "  if ismain then\n"
        "    shield._block_sleep(ms)\n"
        "  else\n"
        "    shield._resume_after(ms); coroutine.yield()\n"
        "  end\n"
        "end",          // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           shd::protected_function_result  // GCOVR_EXCL_LINE (gcov clone
                                           // artifact)
               pfr)                        // GCOVR_EXCL_LINE (gcov
                                           // clone artifact)
        -> shd::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
}

void register_task_api(shd::table& shield, LuaServiceManager* manager,
                       LuaRuntime* runtime) {
    (void)runtime;

    shield.set_function(
        "fork",
        [manager](shd::this_state state,
                  shd::function fn) -> shd::variadic_results {
            shd::state_view lua(state);
            shd::variadic_results results;
            const std::string service_id = manager->current_service_id();

            // Check fork limit.
            if (manager->pending_task_count(service_id) >= kForkLimit) {
                results.push_back(shd::make_object(lua, shd::nil));
                shd::this_state ts(fn.lua_state());
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
            // Written as a plain if instead of a ternary: the ternary's
            // multi-line arms produced pseudo line entries the counters
            // never landed on (coverage artifact).
            lua_State* fn_main = nullptr;
            if (fn_state != nullptr) {  // GCOVR_EXCL_BR_LINE (defensive: sol's
                                        // function conversion always hands a
                                        // live lua_State, so this arm never
                                        // runs)
                fn_main = shd::main_thread(fn_state);
            }
            // Branch-only exclusions on the arms the suites cannot reach:
            // fn_state == nullptr (guarded above), fn_main == fn_state (a
            // shd::function holding LUA_NOREF — a converted Lua function
            // always has a registry reference), and a shd::function holding
            // LUA_NOREF. Both semantic paths run — the coroutine fork
            // re-anchors (ForkAnchorsInsideHandlerAndFromMainThread) and the
            // main-thread fork skips it.
            if (fn_main != nullptr &&
                fn_main != fn_state &&  // GCOVR_EXCL_BR_LINE (defensive: see
                                        // note above)
                fn.registry_index() !=
                    LUA_NOREF) {  // GCOVR_EXCL_BR_LINE (defensive: see note
                                  // above)
                fn =
                    shd::function(fn_main, shd::ref_index(fn.registry_index()));
            }
            uint64_t task_id = manager->enqueue_forked_task(
                service_id,
                // GCOVR_EXCL_START (unreachable: shield.fork always hands a
                // valid raw_fn, so the actor dispatches through the coroutine
                // path and never runs this plain error wrapper)
                []() {
                    auto& log = shield::log::get_logger("lua");
                    SHIELD_LOG_ERROR(log, "task error: fork body missing");
                },
                // GCOVR_EXCL_STOP
                fn);  // raw_fn for coroutine wrapping
            results.push_back(shd::make_object(lua, task_id));
            return results;
        });
}

void register_config_api(shd::table& shield) {
    shield.set_function(
        "config",
        [](shd::this_state state, std::string key,
           std::optional<shd::object> default_value) -> shd::object {
            shd::state_view lua(state);
            auto& config = shield::config::global_config();
            if (!config.has(key)) {
                if (default_value) {
                    return *default_value;
                }
                return shd::make_object(lua, shd::nil);
            }

            const auto value = config.get_string(key, "");
            if (value == "true") {
                return shd::make_object(lua, true);
            }
            if (value == "false") {
                return shd::make_object(lua, false);
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
                        return shd::make_object(lua, parsed);
                    }
                } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch-entry
                           // pseudo-arc)
                    const std::exception&) {  // (compiler
                                              // artifact: exception-edge arc;
                                              // the catch path runs under the
                                              // 'e999xx' config test)
                }
            } else {
                try {
                    size_t pos = 0;
                    const long long parsed = std::stoll(value, &pos);
                    if (pos == value.size()) {
                        return shd::make_object(lua, parsed);
                    }
                } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch-entry
                           // pseudo-arc)
                    const std::exception&) {  // (compiler
                                              // artifact: exception-edge arc;
                                              // runs under the 'zzz' config
                                              // test)
                }

                try {
                    size_t pos = 0;
                    const double parsed = std::stod(value, &pos);
                    if (pos == value.size()) {
                        return shd::make_object(lua, parsed);
                    }
                } catch (  // GCOVR_EXCL_BR_LINE (compiler artifact: catch-entry
                           // pseudo-arc)
                    const std::exception&) {  // (compiler
                                              // artifact: exception-edge arc;
                                              // runs under the 'zzz' config
                                              // test)
                }
            }

            return shd::make_object(lua, value);
        });
}

void register_log_api(shd::table& shield, LuaServiceManager* manager) {
    auto& log = shield::log::get_logger("lua");
    shd::state_view lua(shield.lua_state());
    auto log_table = lua.create_table();

    // Helper: build log message with service context prefix.
    auto build_msg = [manager](shd::object value) -> std::string {
        std::string msg = lua_to_json(value).dump();
        const std::string sid = manager->current_service_id();
        if (sid.empty()) return msg;
        return "[" + sid + "] " + msg;
    };

    log_table.set_function("debug", [&log, build_msg](shd::object value) {
        SHIELD_LOG_DEBUG(log, build_msg(value));
    });
    log_table.set_function("info", [&log, build_msg](shd::object value) {
        SHIELD_LOG_INFO(log, build_msg(value));
    });
    log_table.set_function("warn", [&log, build_msg](shd::object value) {
        SHIELD_LOG_WARNING(log, build_msg(value));
    });
    log_table.set_function("error", [&log, build_msg](shd::object value) {
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

void register_timer_api(shd::table& shield, LuaServiceManager* manager,
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
void register_client_identity_api(shd::state_view lua) {
    // Identity usertypes: no Lua-side constructor (the runtime creates
    // them), read-only properties, and ClientContext.ref materializing the
    // matching ClientRef. Registered under "shd.ClientContext" /
    // "shd.ClientRef" so they appear as literal dotted globals
    // (_G["shd.ClientContext"]) per the coverage test expectation.
    shd::new_usertype<ClientContextBox>(
        lua, "shd.ClientContext", "new", shd::no_constructor, "player_id",
        [](const ClientContextBox& box) { return box.data.player_id; },
        "session_id",
        [](const ClientContextBox& box) { return box.data.session_id; },
        "session_epoch",
        [](const ClientContextBox& box) { return box.data.session_epoch; },
        "protocol_profile_id",  // GCOVR_EXCL_LINE (gcov clone artifact:
                                // no suite caller reads the profile id)
        [](const ClientContextBox& box) {         // GCOVR_EXCL_LINE (gcov clone
                                                  // artifact)
            return box.data.protocol_profile_id;  // GCOVR_EXCL_LINE (gcov
                                                  // clone artifact)
        },  // GCOVR_EXCL_LINE (gcov clone artifact)
        "gateway",
        [](const ClientContextBox& box) { return box.data.gateway_address; },
        "ref",
        [](const ClientContextBox& box) { return ClientRefBox{box.data}; });
    shd::new_usertype<ClientRefBox>(
        lua, "shd.ClientRef", "new", shd::no_constructor, "player_id",
        [](const ClientRefBox& box) { return box.data.player_id; },
        "session_id",
        [](const ClientRefBox& box) { return box.data.session_id; },
        "session_epoch",
        [](const ClientRefBox& box) { return box.data.session_epoch; },
        "protocol_profile_id",
        [](const ClientRefBox& box) { return box.data.protocol_profile_id; },
        "gateway",
        [](const ClientRefBox& box) { return box.data.gateway_address; });

#ifdef SHIELD_ENABLE_PLAYER
    shd::new_usertype<PlayerRefBox>(
        lua, "PlayerRef", "new", shd::no_constructor, "uid",
        shd::property([](const PlayerRefBox& box) { return box.data.uid; }),
        "node_id",
        shd::property([](const PlayerRefBox& box) { return box.data.node_id; }),
        "service_id", shd::property([](const PlayerRefBox& box) {
            return box.data.service_id;
        }),
        "epoch",
        shd::property([](const PlayerRefBox& box) { return box.data.epoch; }));
#endif

    lua.set_function(
        "__shield_make_client_context",
        [](shd::this_state s, std::uint64_t session_id,
           std::uint32_t session_epoch, std::string player_id,
           std::string gateway_address, std::string protocol_profile_id) {
            // clang-format off
            return shd::make_object(  // GCOVR_EXCL_BR_LINE (compiler
                                      // artifact: inlined shd::make_object
                                      // boxing arcs on the call line)
                s, ClientContextBox{ClientContextData{  // GCOVR_EXCL_BR_LINE (same make_object boxing artifact as the call line above)
                       std::move(gateway_address), session_id, session_epoch,
                       std::move(player_id), std::move(protocol_profile_id)}});
            // clang-format on
        });
}

// shield.client.* (bind / close) and the _client_* primitives behind them.
// The bind primitive suspends the caller coroutine exactly like _coro_call:
// the gateway actor completes the session through complete_call, and the
// Lua wrapper resumes with (true, client_ref) or (false, error_table).
void register_client_api(shd::table& shield, LuaServiceManager* manager) {
    shield.set_function(
        "_client_bind",
        [manager](shd::this_state state, shd::object client,
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
                manager->suspend_for_call(state, timeout_ms, target_service);
            caf::actor gateway = manager->gateway_actor(data.gateway_address);
            if (gateway == nullptr) {
                // GCOVR_EXCL_BR_START (compiler artifact: nlohmann
                // init-list construction arcs; path executed by
                // ClientPrimitivesWithoutGateway)
                manager->complete_call(  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: nlohmann init-list
                                         // construction arcs; path executed by
                                         // ClientPrimitivesWithoutGateway)
                    session, false,
                    nlohmann::json::array(
                        {nlohmann::json::
                             object(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // nlohmann init-list construction arcs)
                                 {{"code", "client_rpc.epoch_expired"},
                                  {"message", "gateway not found: " +
                                                  data.gateway_address}})}));
                // GCOVR_EXCL_BR_STOP
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
        });  // GCOVR_EXCL_BR_LINE (compiler artifact: shd argument-conversion
             // template arcs at the set_function boundary)

    shield.set_function(
        "_client_close",
        [manager](shd::object client, std::string reason) -> bool {
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
        [manager](shd::object client, uint32_t route_id,
                  shd::object payload) -> bool {
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
            if (payload.is<shd::table>()) {
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

    shd::state_view lua(shield.lua_state());
    shd::table client = lua.create_table();
    shield["client"] = client;
    // The wrapper bodies resolve the shield table as a global at CALL time
    // (register_full_shield_api assigns lua["shield"] afterwards), so the
    // chunk only returns the functions instead of touching the global.
    shd::function bind_fn =
        shd::safe_script(
            lua,
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
               shd::protected_function_result  // GCOVR_EXCL_LINE (gcov clone
                                               // artifact)
                   pfr)                        // GCOVR_EXCL_LINE (gcov
                                               // clone artifact)
            -> shd::protected_function_result {
                return pfr;
            })  // GCOVR_EXCL_LINE
            .get<shd::function>(0);
    client["bind"] = bind_fn;
    shd::function close_fn =
        shd::safe_script(
            lua,
            "return function(client, reason)\n"
            "  return shield._client_close(client, reason or 'kicked')\n"
            "end\n",        // GCOVR_EXCL_LINE (gcov continuation artifact)
            [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
               shd::protected_function_result  // GCOVR_EXCL_LINE (gcov clone
                                               // artifact)
                   pfr)                        // GCOVR_EXCL_LINE (gcov
                                               // clone artifact)
            -> shd::protected_function_result {
                return pfr;
            })  // GCOVR_EXCL_LINE
            .get<shd::function>(0);
    client["close"] = close_fn;
}

// Registers one shield.client_rpc.<name> helper bound to a server-to-client
// descriptor route. Called per service VM at spawn time (after the
// descriptor table is compiled).
void register_client_rpc_helper(lua_State* L, LuaServiceManager* manager,
                                std::string_view name, uint32_t route_id) {
    shd::state_view lua(L);
    shd::table shield = lua["shield"];
    shd::table client_rpc = shield["client_rpc"];
    // Reverse map for the player client_message guard (route_id -> name);
    // absent names degrade to "route_<id>" at the guard call site.
    {
        shd::object names_obj = shield["_client_route_names"];
        if (!names_obj.valid() || !names_obj.is<shd::table>()) {
            names_obj = lua.create_table();
            shield["_client_route_names"] = names_obj;
        }
        names_obj.as<shd::table>()[route_id] = std::string(name);
    }
    client_rpc.set_function(
        std::string(name),
        [manager, route_id](shd::object client, shd::object payload) -> bool {
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
            if (payload.is<shd::table>()) {
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
void register_cluster_api(shd::table& shield, LuaServiceManager* manager) {
    shd::state_view lua(shield.lua_state());
    auto cluster = lua.create_table();

    // shield.cluster.query(node_id, service_name) -> service_id or nil, error
    cluster.set_function(  // GCOVR_EXCL_LINE (gcov continuation artifact)
        "query",
        [](shd::this_state state,  // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string node_id,    // GCOVR_EXCL_LINE (lambda entry artifact)
           std::string service_name) -> shd::variadic_results {
            shd::state_view lua(state);
            shd::variadic_results results;

            auto* cluster_manager = shield::cluster::global_cluster_manager();
            if (!cluster_manager) {
                results.push_back(shd::make_object(lua, shd::nil));
                results.push_back(make_error(state, "module_unavailable",
                                             "shield_cluster is not enabled"));
                return results;
            }

            const auto reachable =
                cluster_manager->check_node_reachable(node_id);
            if (!reachable.empty()) {
                results.push_back(shd::make_object(lua, shd::nil));
                results.push_back(
                    make_error(state, reachable,
                               "cluster node is not reachable: " + node_id));
                return results;
            }

            auto service_id =
                cluster_manager->query_remote(node_id, service_name);
            if (service_id.empty()) {
                results.push_back(shd::make_object(lua, shd::nil));
                results.push_back(
                    make_error(state, "service_not_found",
                               "remote service not found: " + service_name));
                return results;
            }

            results.push_back(shd::make_object(lua, service_id));
            results.push_back(shd::make_object(lua, shd::nil));
            return results;
        });

    // shield.cluster.nodes() -> table of node info
    cluster.set_function("nodes", [](shd::this_state state) -> shd::table {
        shd::state_view lua(state);
        shd::table nodes = lua.create_table();
        auto* cluster_manager = shield::cluster::global_cluster_manager();
        if (!cluster_manager) {
            return nodes;
        }
        int index = 1;
        for (const auto& node : cluster_manager->nodes()) {
            shd::table entry = lua.create_table();
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
    });  // GCOVR_EXCL_LINE (function-exit arc artifact of the peers/nodes
         // lambda)

    // shield.cluster.node_id() -> this node's ID
    cluster.set_function(  // GCOVR_EXCL_LINE (gcov continuation artifact)
        "node_id",
        [](shd::this_state state)  // GCOVR_EXCL_LINE (lambda entry artifact)
        -> std::optional<std::string> {  // GCOVR_EXCL_LINE (lambda entry
                                         // artifact)
            auto* cluster_manager = shield::cluster::global_cluster_manager();
            if (!cluster_manager || cluster_manager->node_id().empty()) {
                return shd::nullopt;
            }
            return cluster_manager->node_id();
        });

    cluster.set_function("node_epoch", []() -> std::optional<std::string> {
        auto* cluster_manager = shield::cluster::global_cluster_manager();
        if (!cluster_manager) {
            return shd::nullopt;
        }
        // Serialized as a decimal string: uint64 does not survive the
        // Lua number (double, 53-bit mantissa) round-trip.
        return std::to_string(cluster_manager->node_epoch());
    });

    shield["cluster"] = cluster;
}
#endif

void register_full_shield_api(lua_State* L, LuaServiceManager* manager,
                              LuaRuntime* runtime) {
    shd::state_view lua(L);
    // Initialize HTTP client (libcurl global state).
    shield::net::HttpClient::initialize();

    // Register usertypes (ServiceHandle is a shd usertype since B1)
    ServiceHandle::register_usertype(shd::state_view(L));
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
    {
        // Crypto registration binds through the shd layer
        // (shield/lua/binding.hpp). The shd::table registry reference below
        // mutates the same underlying Lua table this function hands to
        // scripts.
        lua_State* crypto_state = shield.lua_state();
        shield.push();
        shd::table crypto_target(crypto_state, -1);
        lua_pop(crypto_state, 1);
        register_crypto_api(crypto_target);
    }
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
    // Skipped entirely when the os library is sandboxed away
    // (lua.sandbox.allow_os=false): there is no table to hook, and business
    // code cannot reach os.time in the first place. Note the optional-based
    // read: constructing shd::table from a nil proxy trips shd's Debug
    // type check before .valid() could run.
    if (std::optional<shd::table> os_opt =
            lua["os"].get<std::optional<shd::table>>()) {
        shd::table os_t = *os_opt;
        shd::function orig_time = os_t["time"];
        shd::function orig_date = os_t["date"];
        // os.time(): no-arg → business clock seconds; with table → original.
        os_t.set_function("time",
                          [manager, orig_time = std::move(orig_time)](
                              std::optional<shd::table> t) -> shd::object {
                              shd::state_view lua(orig_time.lua_state());
                              if (t.has_value()) {
                                  return orig_time(t.value());
                              }
                              return shd::make_object(
                                  lua, manager->clock_now_seconds());
                          });
        // os.date(fmt): no time → business clock; with time → original.
        os_t.set_function(
            "date",
            [manager, orig_date = std::move(orig_date)](
                std::optional<std::string> fmt,
                std::optional<double> t) -> shd::object {
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
    shd::safe_script(
        lua,
        "function __shield_run_handler(handler, args)\n"
        // GCOVR_EXCL_STOP
        "  return coroutine.create(function()\n"
        "    return handler(table.unpack(args, 1, args.n or #args))\n"
        "  end)\n"
        "end",          // GCOVR_EXCL_LINE (gcov continuation artifact)
        [](lua_State*,  // GCOVR_EXCL_LINE (gcov clone artifact)
           shd::protected_function_result  // GCOVR_EXCL_LINE (gcov clone
                                           // artifact)
               pfr)                        // GCOVR_EXCL_LINE (gcov
                                           // clone artifact)
        -> shd::protected_function_result { return pfr; });  // GCOVR_EXCL_LINE
}

}  // namespace shield::lua
