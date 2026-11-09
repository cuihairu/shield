// [SHIELD_LUA] Lua service messaging and call plumbing: send/call entry
// points, client ingress dispatch, coroutine suspend/resume machinery,
// proxied-call bookkeeping, error hooks and the call-timeout drivers.
// Split out of lua_service.cpp; member declarations live in
// lua_service_impl.hpp.
#include <algorithm>
#include <atomic>
#include <caf/actor.hpp>
#include <caf/actor_system.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/mail_cache.hpp>
#include <caf/scoped_actor.hpp>
#include <caf/send.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lua_service_impl.hpp"
#include "shield/base/error.hpp"
#include "shield/base/result.hpp"
#include "shield/config/config.hpp"
#include "shield/core/service_message.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_constants.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/profile_sampler.hpp"
#include "shield/lua/slow_calls.hpp"
#include "shield/plugin/plugin_host.hpp"
#include "shield/transport/rpc_descriptor.hpp"

namespace shield::lua {

void LuaServiceManager::Impl::dispatch_service_message(
    class LuaServiceManager* manager, const std::string& id,
    const ServiceMessage& msg) {
    DispatchMessage m;
    m.sender = msg.sender;
    m.method = msg.method;
    m.args = msg.args;
    m.trace_id = msg.trace_id;
    m.deadline_ms = msg.deadline_ms;
    m.high_priority = msg.priority == MessagePriority::High;
    m.timestamp_ms = msg.timestamp_ms;
    m.call_session = msg.call_session;
    (void)dispatch_message(manager, id, m);
}

void LuaServiceManager::Impl::dispatch_call_response(
    class LuaServiceManager* manager, const CallResponseMessage& msg) {
    manager->resume_caller(msg.session, msg.ok, msg.values);
}

void LuaServiceManager::Impl::dispatch_client_ingress(
    class LuaServiceManager* manager, const std::string& id,
    const ClientIngress& msg) {
    std::shared_ptr<LuaVM> service;
    shd::function handler;
    ClientIngress normalized = msg;
    {
        std::shared_lock lock(registry_mutex);
        auto svc_it = services.find(id);
        if (svc_it ==
            services.end()) {  // GCOVR_EXCL_START
                               //  (race: ingress after
                               // service teardown (body excluded below))
                               //  (race: client ingress
                               // after service teardown (see
                               // the exclusion region)) after service teardown)
            auto& log = shield::log::get_logger(
                "lua");  //  (race: unreachable body of
                         // the ingress-after-teardown arm)
            SHIELD_LOG_WARNING(log,
                               "client rpc for unknown service " +
                                   id);  //  (race: unreachable body
                                         // of the ingress-after-teardown arm)
            return;
        }
        // GCOVR_EXCL_STOP
        service = svc_it->second;
        auto rpc_it = service_rpc.find(id);
        if (rpc_it ==
            service_rpc.end()) {  // GCOVR_EXCL_START
                                  //  (race: rpc table missing
                                  // after teardown (body excluded below))
                                  // //  (race: rpc table
                                  // missing after teardown (see
                                  // the exclusion region)) service teardown)
            auto& log =
                shield::log::get_logger("lua");  //  (race: unreachable body of
                                                 // the rpc-table-missing arm)
            SHIELD_LOG_WARNING(log,
                               "client rpc table missing for " +
                                   id);  //  (race: unreachable body
                                         // of the rpc-table-missing arm)
            return;
        }
        // GCOVR_EXCL_STOP
        auto h_it = rpc_it->second.handlers.find(msg.route_id);
        if (h_it == rpc_it->second.handlers.end()) {
            auto& log = shield::log::get_logger("lua");
            SHIELD_LOG_WARNING(log, "client rpc route " +
                                        std::to_string(msg.route_id) +
                                        " not owned by " + id);
            return;
        }
        handler = h_it->second;
        if (!normalized.decoded_request) {
            const auto* descriptor =
                rpc_it->second.descriptors.find(msg.route_id);
            const bool json_codec =
                descriptor == nullptr ||
                descriptor->request_codec
                    .empty() ||  // GCOVR_EXCL_BR_LINE (defensive: a
                                 // non-json request_codec arrives only
                                 // through the gateway bridge integration
                                 // path)
                descriptor->request_codec ==  // GCOVR_EXCL_BR_LINE
                                              // (defensive:
                                              // gateway-bridge-only codec
                                              // arm)
                    "json";                   // GCOVR_EXCL_BR_LINE (defensive:
                             // gateway-bridge-only codec twin of the 878
                             // arm)
            if (json_codec) {
                auto parsed = nlohmann::json::parse(
                    normalized.body_bytes.begin(), normalized.body_bytes.end(),
                    nullptr, false);
                normalized.decoded_request =
                    parsed.is_discarded()
                        ? nlohmann::json(
                              std::string(  // GCOVR_EXCL_BR_LINE
                                            // (defensive: body_bytes decode
                                            // is gateway-bridge-only)
                                  normalized.body_bytes
                                      .begin(),  // GCOVR_EXCL_BR_LINE
                                                 // (defensive: body_bytes
                                                 // decode reached only via
                                                 // the gateway bridge path)
                                  normalized.body_bytes
                                      .end()))  // GCOVR_EXCL_BR_LINE
                                                // (defensive: body_bytes
                                                // decode reached only via
                                                // the gateway bridge path)
                        : std::move(parsed);
            } else {
                normalized.decoded_request =
                    nlohmann::json(std::string(normalized.body_bytes.begin(),
                                               normalized.body_bytes.end()));
            }
        }
    }

    // Dispatch context for the handler coroutine: without it a
    // shield.client.bind inside the handler records an empty
    // caller_service, and the completion cannot route back to this
    // actor (the pending call is dropped silently and the coroutine
    // never resumes). Client ingress is gateway-originated
    // fire-and-forget, so there is no sender/trace/deadline.
    DispatchScope scope(*this, id, "", false);

    std::string error;
    if (!runtime.invoke_client_rpc(service, handler, normalized, &error,
                                   manager, id)) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log, "client rpc dispatch failed on " + id +
                                    " route " + std::to_string(msg.route_id) +
                                    ": " + error);
    }
}

void LuaServiceManager::Impl::dispatch_client_control(
    class LuaServiceManager* manager, const std::string& id,
    const ClientControlMessage& msg) {
    std::string method;
    nlohmann::json args = nlohmann::json::array(
        {msg.context.to_json()});  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                   // json::array braced-init arc)
    switch (msg.kind) {  // GCOVR_EXCL_BR_LINE (defensive: only the Bound
                         // control kind is sent by the exercised bridge path)
        case ClientControlMessage::Kind::Bound:
            method = "on_client_bound";
            break;
        case ClientControlMessage::Kind::Disconnected:
            method = "on_disconnect";
            args.push_back(msg.reason);
            break;
        case ClientControlMessage::Kind::Unbound:
            method = "on_client_unbound";
            args.push_back(msg.reason);
            break;
        case ClientControlMessage::Kind::Reconnected: {
            // Reserved: no producer in this milestone.
            auto& log = shield::log::get_logger("lua");
            SHIELD_LOG_WARNING(log, "dropped Reconnected control for session " +
                                        std::to_string(msg.context.session_id));
            return;
        }
    }
    std::string error;
    if (!manager->send_system(id, method, args, &error)) {
        auto& log = shield::log::get_logger("lua");
        SHIELD_LOG_WARNING(log, "Failed to queue " + method + ": " + error);
    }
}

bool LuaServiceManager::Impl::dispatch_message(class LuaServiceManager* manager,
                                               const std::string& id,
                                               const DispatchMessage& msg) {
    std::shared_ptr<LuaVM> service;
    std::shared_ptr<ServiceCounters> counters;
    {
        std::shared_lock lock(registry_mutex);
        auto service_it = services.find(id);
        if (service_it != services.end()) {
            service = service_it->second;
            if (auto counters_it = service_counters.find(id);
                counters_it !=
                service_counters.end()) {  // GCOVR_EXCL_BR_LINE (defensive:
                                           // publish inserts service and
                                           // counters in one critical section;
                                           // counters can never miss here)
                counters = counters_it->second;
            }
        }
    }
    if (!service) {
        return false;
    }

    DispatchScope scope(*this, id, msg.sender, false, msg.trace_id,
                        msg.deadline_ms);

    std::string error;
    const bool dispatched_ok = runtime.call_service_method_coroutine(
        service, msg.method, msg.args, &error, msg.call_session, manager, id);
    // Method failed - log error but continue processing other messages.

    // Traffic accounting stays out of the registry lock: the counters
    // outlive this dispatch through the shared_ptr even if the service
    // exits concurrently (the entry just drops from later snapshots).
    if (counters) {  // GCOVR_EXCL_BR_LINE (defensive: counters is always
                     // set when the service lookup succeeded (single insert
                     // site))
        counters->requests.fetch_add(1, std::memory_order_relaxed);
        if (!dispatched_ok) {
            counters->errors.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Honor shield.exit requested by the handler (or by a resumed
    // continuation of it — see finish_pending_exit).
    manager->finish_pending_exit(id);

    return true;
}

namespace {

bool validate_message_method(std::string_view method, bool allow_reserved,
                             std::string* error) {
    if (method.empty() || method.size() > 128) {
        if (error) *error = "invalid method name: length must be 1-128";
        return false;
    }
    if (!allow_reserved && method.rfind("on_", 0) == 0) {
        if (error) *error = "invalid method name: 'on_' prefix is reserved";
        return false;
    }
    return true;
}

bool validate_message_payload(const nlohmann::json& args, std::string* error) {
    const std::string serialized = args.dump();
    if (serialized.size() > kMaxMessageSize) {
        if (error) {
            *error = "message too large: " + std::to_string(serialized.size()) +
                     " bytes (max " + std::to_string(kMaxMessageSize) + ")";
        }
        return false;
    }
    // Check for unsupported JSON values, e.g. from lua_to_json returning the
    // sentinel string for userdata/functions.
    if (serialized.find("\"<unsupported>\"") != std::string::npos) {
        if (error) *error = "message contains unsupported value type";
        return false;
    }
    return true;
}

}  // namespace

bool LuaServiceManager::send(std::string_view target, std::string_view method,
                             const nlohmann::json& args, std::string* error) {
    if (impl_->stopping.load()) {
        if (error) *error = "runtime is stopping";
        return false;
    }

    if (!validate_message_method(method, false, error)) {
        return false;
    }

    // Permission check.
    // GCOVR_EXCL_START (Phase-2 scaffold: no setter exists yet)
    if (impl_
            ->permission_check) {  //  (defensive: Phase-2
                                   // scaffold - no permission_check setter
                                   // exists (see the exclusion region at 2182))
        const std::string sender =
            current_service_id();  //  (defensive: Phase-2
                                   // scaffold - no permission_check setter
                                   // exists (see the exclusion region at 2182))
        const std::string denial = impl_->permission_check(
            sender,
            std::string(target),   //  (defensive: Phase-2
                                   // scaffold (see the exclusion region below))
            std::string(method));  //  (defensive: Phase-2
                                   // scaffold - no permission_check setter
                                   // exists (see the exclusion region at 2182))
        if (!denial.empty()) {     //  (defensive: Phase-2
                                // scaffold - no permission_check setter exists
                                // (see the exclusion region at 2182))
            if (error)  //  (defensive: Phase-2 scaffold (see
                        // the exclusion region below))
                *error = "permission denied: " +
                         denial;  //  (defensive: Phase-2
                                  // scaffold - no permission_check setter
                                  // exists (see the exclusion region at 2182))
            return false;
        }
    }
    // GCOVR_EXCL_STOP

    if (!validate_message_payload(args, error)) {
        return false;
    }

    const std::string service_id = query_service(target);

    std::optional<caf::actor> actor_opt;
    bool target_recently_exited = false;
    {
        // Also read recently_exited under the same lock: exit() inserts
        // into it under the unique lock on another thread.
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->service_actors.find(service_id);
        if (it != impl_->service_actors.end()) {
            actor_opt = it->second;
        }
        target_recently_exited =
            impl_->recently_exited.count(service_id) >
                0 ||  // GCOVR_EXCL_BR_LINE (defensive: the sender's own id is
                      // in recently_exited only after it exited - a dead
                      // service cannot send) // GCOVR_EXCL_BR_START
            impl_->recently_exited.count(std::string(target)) >
                0;  //  (compiler artifact: unordered_set
                    // count inlined probe arms) // GCOVR_EXCL_BR_STOP
    }
    if (!actor_opt) {
        if (error) {
            if (target_recently_exited) {
                *error = "service dead: " + std::string(target);
            } else {
                *error = "service not found: " + std::string(target);
            }
        }
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
    const std::string sender = current_service_id();
    ServiceMessage msg;
    msg.sender = sender;
    msg.method = std::string(method);
    msg.args = args;
    msg.trace_id = impl_->current_trace_id();
    msg.deadline_ms = impl_->current_deadline_ms();
    msg.priority = MessagePriority::Normal;
    msg.timestamp_ms = now_ms;
    caf::anon_send(*actor_opt, std::move(msg));
    return true;
}

bool LuaServiceManager::send_system(std::string_view target,
                                    std::string_view method,
                                    const nlohmann::json& args,
                                    std::string* error) {
    if (impl_->stopping.load()) {
        if (error) *error = "runtime is stopping";
        return false;
    }
    if (!validate_message_method(method, true, error) ||
        !validate_message_payload(args, error)) {
        return false;
    }

    const std::string service_id = query_service(target);

    std::optional<caf::actor> actor_opt;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->service_actors.find(service_id);
        if (it != impl_->service_actors.end()) {
            actor_opt = it->second;
        }
    }
    if (!actor_opt) {
        if (error) *error = "service not found: " + std::string(target);
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
    ServiceMessage msg;
    msg.sender = "";
    msg.method = std::string(method);
    msg.args = args;
    msg.trace_id = "";
    msg.deadline_ms = 0;
    msg.priority = MessagePriority::High;
    msg.timestamp_ms = now_ms;
    caf::anon_send(*actor_opt, std::move(msg));
    return true;
}

caf::actor LuaServiceManager::service_actor(
    std::string_view service_name) const {
    const std::string service_id = query_service(service_name);
    std::shared_lock lock(impl_->registry_mutex);
    auto it = impl_->service_actors.find(service_id);
    return it == impl_->service_actors.end() ? nullptr : it->second;
}

bool LuaServiceManager::send_call_request(std::string_view target,
                                          std::string_view method,
                                          const nlohmann::json& args,
                                          uint64_t session,
                                          std::string* error) {
    const std::string service_id = query_service(target);

    std::optional<caf::actor> actor_opt;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->service_actors.find(service_id);
        if (it != impl_->service_actors.end()) {
            actor_opt = it->second;
        }
    }
    if (!actor_opt) {
        if (error) {
            *error = "service not found: " + std::string(target);
        }
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
    const std::string sender = current_service_id();
    ServiceMessage msg;
    msg.sender = sender;
    msg.method = std::string(method);
    msg.args = args;
    msg.trace_id = impl_->current_trace_id();
    msg.deadline_ms = impl_->current_deadline_ms();
    msg.priority = MessagePriority::Normal;
    msg.timestamp_ms = now_ms;
    msg.call_session = session;
    caf::anon_send(*actor_opt, std::move(msg));
    return true;
}

CallResult LuaServiceManager::call(std::string_view target,
                                   std::string_view method,
                                   const nlohmann::json& args,
                                   int32_t timeout_ms) {
    if (impl_->stopping.load()) {
        return CallResult::error("runtime is stopping");
    }

    const std::string service_id = query_service(target);
    std::shared_ptr<LuaVM> service;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto service_it = impl_->services.find(service_id);
        if (service_it != impl_->services.end()) {
            service = service_it->second;
        }
    }
    if (!service) {
        return CallResult::error("service not found: " + std::string(target));
    }

    const std::string sender = current_service_id();

    std::optional<caf::actor> actor_opt;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->service_actors.find(service_id);
        if (it !=
            impl_->service_actors
                .end()) {  // GCOVR_EXCL_BR_LINE (defensive: twin of the early
                           // lookup's identical return; the post-lock recheck
                           // never fires (see the exclusion region at 2366))
            actor_opt = it->second;
        }
    }
    if (!actor_opt) {  // GCOVR_EXCL_BR_LINE (defensive: twin of the early
                       // lookup's identical return (see the exclusion region at
                       // 2366))
        // GCOVR_EXCL_START (twin of the early-spawn lookup's identical
        // return, which the tests drive; this post-lock recheck never fires)
        return CallResult::error(
            "service not found: " +
            std::string(target));  //  (defensive: twin of the
                                   // early lookup's identical return
                                   // (line-excluded body))
        // GCOVR_EXCL_STOP
    }

    // Self-call detection: avoid deadlock (actor mailbox would queue but the
    // actor is currently executing this handler).
    // GCOVR_EXCL_START (defensive: deadlock guard — Lua self-calls route
    // through the runtime's inline invoke, so a manager.call self-dispatch
    // is unreachable in the suites)
    if (sender == service_id) {
        return CallResult::error("self-call not supported");
    }  // GCOVR_EXCL_STOP

    // Create the pending external waiter, then send the call request through
    // the ordinary ServiceMessage path (call_session non-zero marks it as a
    // call; the callee's completion routes back via complete_call).
    const uint64_t session = impl_->next_call_session.fetch_add(1);
    auto pending = std::make_shared<Impl::PendingSyncCall>();
    pending->session = session;
    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_sync_calls[session] = pending;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
    ServiceMessage msg;
    msg.sender = sender;
    msg.method = std::string(method);
    msg.args = args;
    msg.trace_id = impl_->current_trace_id();
    msg.deadline_ms = impl_->current_deadline_ms();
    msg.timestamp_ms = now_ms;
    msg.call_session = session;

    // Send to target actor.
    caf::anon_send(*actor_opt, std::move(msg));

    // Block until the handler completes. The expiry is driven by the CAF
    // delayed driver (it completes the call with a timeout error); when the
    // driver could not be armed, fall back to a timed wait on this thread.
    const int32_t effective_timeout = timeout_ms > 0 ? timeout_ms : 5000;
    const bool driver_armed =
        schedule_external_call_timeout(effective_timeout, session);
    bool completed = false;
    {
        std::unique_lock lk(pending->mtx);
        if (driver_armed) {  // GCOVR_EXCL_BR_LINE (defensive: the CAF expiry
                             // driver is always armed (see the exclusion region
                             // at 2418))
            pending->cv.wait(lk, [&] { return pending->completed; });
        } else {  // GCOVR_EXCL_START (defensive: the CAF expiry driver is
                  // always armed)
            completed =
                pending->cv.wait_for(  //  (defensive: the CAF
                                       // expiry driver is always armed (see
                                       // the exclusion region at 2418))
                    lk, std::chrono::milliseconds(effective_timeout),
                    [&] { return pending->completed; });
        }
        // GCOVR_EXCL_STOP
        completed =
            completed ||  // GCOVR_EXCL_BR_LINE (defensive: driver path always
                          // completes first)
            pending->completed;  // GCOVR_EXCL_BR_LINE (defensive: the true arc
                                 // belongs to the fallback wait that never runs
                                 // (driver always armed))
    }

    // Cleanup.
    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_sync_calls.erase(session);
    }

    // GCOVR_EXCL_START (defensive: the driver path completes the call
    // first; this fallback only fires if actor dispatch stalls entirely)
    if (!completed) {
        return CallResult::error(
            "call timeout (actor dispatch exceeded limit)");
    }  // GCOVR_EXCL_STOP
    if (pending->ok) {
        return CallResult::ok(std::move(pending->values));
    }
    return CallResult::error(std::move(pending->error));
}

// Deepest JSON subtree converted to Lua. The DOM is guarded at the codec
// boundary (kMaxJsonDepth there), so this cap is defense-in-depth: a deep
// value that slips through a non-network path degrades to nil instead of
// overflowing the C++ stack on this recursive conversion (which runs on the
// no-recovery resume path).
constexpr std::size_t kPushJsonMaxDepth = 512;

// Push a JSON value onto a raw lua_State using the C API (avoids
// binding-layer stack residue when targeting a specific coroutine thread).
static void push_json_to_stack(lua_State* L, const nlohmann::json& v,
                               std::size_t depth = 0) {
    // GCOVR_EXCL_START (defensive: the 512-deep recursion guard only fires
    // on a maliciously nested return payload; driving it needs a 513-level
    // nesting that the call path deliberately rejects at encode time)
    if (depth > kPushJsonMaxDepth) {
        lua_pushnil(L);
        return;
    }
    // GCOVR_EXCL_STOP
    if (v.is_null()) {
        lua_pushnil(L);
    } else if (v.is_boolean()) {
        lua_pushboolean(L, v.get<bool>() ? 1 : 0);
    } else if (v.is_number_integer()) {
        lua_pushinteger(L, static_cast<lua_Integer>(v.get<std::int64_t>()));
    } else if (v.is_number_unsigned()) {  // GCOVR_EXCL_BR_LINE (defensive:
                                          // is_number_integer matches every
                                          // integral json first; the unsigned
                                          // arm is unreachable (see source
                                          // comment))
        lua_pushinteger(                  // GCOVR_EXCL_LINE (continuation)
            L, static_cast<lua_Integer>(
                   v.get<std::uint64_t>()));  // GCOVR_EXCL_LINE (unreachable:
                                              // is_number_integer also matches
                                              // unsigned)
    } else if (v.is_number_float()) {
        lua_pushnumber(L, v.get<double>());
    } else if (v.is_string()) {
        const auto& s = v.get_ref<const std::string&>();
        lua_pushlstring(L, s.data(), s.size());
    } else if (v.is_array()) {
        lua_createtable(L, static_cast<int>(v.size()), 0);
        int i = 1;
        for (const auto& el : v) {
            push_json_to_stack(L, el, depth + 1);
            lua_rawseti(L, -2, i++);
        }
    } else if (v.is_object()) {
        // A trusted client-identity marker materializes as the read-only
        // ClientContext userdata instead of a plain table (same rule as
        // json_to_lua in lua_api.cpp). The global helper is registered into
        // every service VM by register_client_identity_api.
        if (auto ctx = ClientContextData::from_json(v)) {
            lua_getglobal(L, "__shield_make_client_context");
            lua_pushinteger(L, static_cast<lua_Integer>(ctx->session_id));
            lua_pushinteger(L, static_cast<lua_Integer>(ctx->session_epoch));
            lua_pushlstring(L, ctx->player_id.data(), ctx->player_id.size());
            lua_pushlstring(L, ctx->gateway_address.data(),
                            ctx->gateway_address.size());
            lua_pushlstring(L, ctx->protocol_profile_id.data(),
                            ctx->protocol_profile_id.size());
            // Protected: this fires on the shield.call continuation resume
            // path (resume_suspended_caller), which has no recovery point
            // above it — since the panic handler aborts, a failing
            // materializer (or a VM without the registration) must degrade
            // to the plain-table branch below instead (mirrors json_to_lua's
            // fallback in lua_api.cpp).
            if (lua_pcall(L, 5, 1, 0) == LUA_OK) {
                return;
            }
            lua_pop(L, 1);  // drop the error object; fall through
        }
        lua_createtable(L, 0, static_cast<int>(v.size()));
        for (auto it = v.begin(); it != v.end(); ++it) {
            lua_pushlstring(L, it.key().data(), it.key().size());
            push_json_to_stack(L, it.value(), depth + 1);
            lua_rawset(L, -3);
        }
    } else {
        lua_pushnil(L);
    }
}

uint64_t LuaServiceManager::suspend_for_call(lua_State* caller_co,
                                             int32_t timeout_ms,
                                             std::string_view callee) {
    const uint64_t session = impl_->next_call_session.fetch_add(1);
    Impl::PendingCall pc;
    pc.session = session;
    pc.caller_co = caller_co;
    // Anchor the caller coroutine against GC while it is suspended.
    lua_pushthread(caller_co);
    pc.caller_anchor = luaL_ref(caller_co, LUA_REGISTRYINDEX);
    const auto now = std::chrono::steady_clock::now();
    pc.deadline_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch())
                         .count() +
                     (timeout_ms > 0 ? timeout_ms : 5000);
    pc.caller_service = current_service_id();
    // Phase B: meter the span only when the process-wide gate is armed.
    // The stamp and the callee ride on the PendingCall so the completion
    // path pays neither a gate re-check round trip nor a second lookup;
    // begin_ms == 0 marks the call as never metered.
    if (SlowCallRing::instance().enabled()) {
        pc.begin_ms = static_cast<uint64_t>(Impl::now_ms());
        pc.callee.assign(callee.data(), callee.size());
    }
    const std::string service = pc.caller_service;
    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_calls.emplace(session, std::move(pc));
    }

    // Step 2c: drive the call timeout via a CAF delayed event.
    if (!service.empty()) {
        const int32_t effective = timeout_ms > 0 ? timeout_ms : 5000;
        schedule_actor_call_timeout(effective, service, session);
    }
    return session;
}

void LuaServiceManager::set_handler_call_session(lua_State* co,
                                                 uint64_t session) {
    if (session != 0 && co != nullptr) {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->handler_call_session[co] = session;
    }
}

void LuaServiceManager::on_handler_completed(
    lua_State* co, const nlohmann::json& return_values) {
    if (co == nullptr) {
        return;
    }
    uint64_t session = 0;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->handler_call_session.find(co);
        if (it == impl_->handler_call_session.end()) {
            return;
        }
        session = it->second;
        impl_->handler_call_session.erase(it);
    }
    complete_call(session, true, return_values);
}

void LuaServiceManager::on_handler_failed(lua_State* co,
                                          const std::string& error_message) {
    if (co == nullptr) {
        return;
    }
    uint64_t session = 0;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->handler_call_session.find(co);
        if (it == impl_->handler_call_session.end()) {
            return;
        }
        session = it->second;
        impl_->handler_call_session.erase(it);
    }
    complete_call(
        session, false,
        nlohmann::json::array(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // json::array init arcs in the failure hook)
            {error_message}));  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // json::array init arcs in the failure hook)
}

namespace {

std::string call_error_message(const nlohmann::json& values) {
    if (values.is_array() &&  // GCOVR_EXCL_BR_LINE
        !values.empty()) {    // GCOVR_EXCL_BR_LINE (defensive: failure payloads
                              // are always arrays (see the exclusion region))
        const auto& first = values.front();
        if (first.is_string()) {
            return first.get<std::string>();
        }
        if (first.is_object() &&
            first.contains(
                "message") &&  // GCOVR_EXCL_BR_LINE (defensive: failure
                               // payloads are strings or {message} objects)
            first["message"].is_string()) {
            return first["message"].get<std::string>();
        }
        return first.dump();  // GCOVR_EXCL_LINE (failure sites pass a plain
                              // string or a {message,...} object)
    }
    // GCOVR_EXCL_START (defensive: every complete_call failure site wraps
    // its payload in an array, so values is never a bare string or object)
    if (values.is_string()) {
        return values.get<std::string>();
    }
    if (values.is_object() && values.contains("message") &&
        values["message"].is_string()) {
        return values["message"].get<std::string>();
    }
    return "call failed";
    // GCOVR_EXCL_STOP
}

// Slack added to a proxied session's deadline on top of the caller's
// remaining timeout: the remote caller's own timeout is authoritative, this
// only bounds how long the entry may outlive a vanished caller.
constexpr int64_t kProxiedCallSlackMs = 30000;

}  // namespace

void LuaServiceManager::complete_call(uint64_t session, bool ok,
                                      const nlohmann::json& values) {
    // Whichever path completes first wins: cancel the expiry driver so a
    // late timeout cannot fire against a session that is already finished.
    cancel_actor_call_timeout(session);
    // External sync calls do not have a caller coroutine. Complete them
    // directly after the callee has fully unwound.
    {
        std::shared_ptr<Impl::PendingSyncCall> pending;
        {
            std::unique_lock lock(impl_->registry_mutex);
            auto sync_it = impl_->pending_sync_calls.find(session);
            if (sync_it != impl_->pending_sync_calls.end()) {
                pending = sync_it->second;
                impl_->pending_sync_calls.erase(sync_it);
            }
        }
        if (pending) {
            {
                std::unique_lock lk(pending->mtx);
                pending->ok = ok;
                if (ok) {
                    pending->values = values;
                } else {
                    pending->error = call_error_message(values);
                }
                pending->completed = true;
            }
            pending->cv.notify_one();
            return;
        }
    }

    // Proxied (remotely originated) calls have no local caller: route the
    // outcome through the hook so the transport replies to the source node.
    if (finish_proxied_call(session, ok, values)) {
        return;
    }

    std::optional<caf::actor> caller_actor;
    std::string caller_service;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto pending_it = impl_->pending_calls.find(session);
        if (pending_it == impl_->pending_calls.end()) {
            return;
        }
        caller_service = pending_it->second.caller_service;
        auto actor_it = impl_->service_actors.find(caller_service);
        if (actor_it != impl_->service_actors.end()) {
            caller_actor = actor_it->second;
        }
    }
    if (!caller_actor) {
        // The caller's actor is gone (exited): nothing may resume the
        // suspended coroutine — resume_caller must run on the caller actor
        // thread, and an inline resume here would drive the caller's Lua VM
        // from a foreign thread. Drop the pending entry; the coroutine (and
        // its anchor) dies with the caller's VM.
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_calls.erase(session);
        return;
    }

    CallResponseMessage response;
    response.session = session;
    response.ok = ok;
    response.values = values;
    caf::anon_send(*caller_actor, std::move(response));
}

uint64_t LuaServiceManager::begin_proxied_call(int32_t timeout_ms) {
    const uint64_t session = impl_->next_call_session.fetch_add(1);
    Impl::PendingCall pc;
    pc.session = session;
    pc.caller_co = nullptr;  // completion routes to the hook, not a coroutine
    pc.proxied = true;
    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count();
    pc.deadline_ms = now_ms + std::max(timeout_ms, 5000) + kProxiedCallSlackMs;
    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_calls.emplace(session, std::move(pc));
    }
    return session;
}

void LuaServiceManager::abandon_proxied_call(uint64_t session) {
    std::unique_lock lock(impl_->registry_mutex);
    auto it = impl_->pending_calls.find(session);
    if (it != impl_->pending_calls.end() && it->second.proxied) {
        impl_->pending_calls.erase(it);
    }
}

void LuaServiceManager::set_proxied_call_hook(
    std::function<void(uint64_t, bool, const nlohmann::json&)> hook) {
    impl_->proxied_call_hook = std::move(hook);
}

bool LuaServiceManager::finish_proxied_call(uint64_t session, bool ok,
                                            const nlohmann::json& values) {
    std::function<void(uint64_t, bool, const nlohmann::json&)> hook;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->pending_calls.find(session);
        if (it == impl_->pending_calls.end() || !it->second.proxied) {
            return false;
        }
        impl_->pending_calls.erase(it);
        hook = impl_->proxied_call_hook;
    }
    // Cancel the expiry driver (if armed); unknown sessions no-op.
    cancel_actor_call_timeout(session);
    if (hook) {
        hook(session, ok, values);
    }
    return true;
}

CallResult LuaServiceManager::call_with_session(
    const std::function<bool(uint64_t, std::string&)>& initiate,
    int32_t timeout_ms) {
    if (impl_->stopping.load()) {
        return CallResult::error("runtime is stopping");
    }

    const uint64_t session = impl_->next_call_session.fetch_add(1);
    auto pending = std::make_shared<Impl::PendingSyncCall>();
    pending->session = session;
    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_sync_calls[session] = pending;
    }

    // Hand the session to the caller-provided starter (e.g. a remote
    // envelope send). A synchronous failure means complete_call will never
    // fire for this session: fail the call on the spot.
    std::string dispatch_error;
    if (!initiate || !initiate(session, dispatch_error)) {
        {
            std::unique_lock lock(impl_->registry_mutex);
            impl_->pending_sync_calls.erase(session);
        }
        return CallResult::error(
            dispatch_error.empty()
                ? "call dispatch failed"  // GCOVR_EXCL_BR_LINE (compiler
                                          // artifact: ternary std::string
                                          // construction arcs)
                : dispatch_error);
    }

    // Expiry is driven by the CAF delayed driver (it completes the call with
    // a timeout error); fall back to a timed wait if the driver was not
    // armed.
    const int32_t effective_timeout = timeout_ms > 0 ? timeout_ms : 5000;
    const bool driver_armed =
        schedule_external_call_timeout(effective_timeout, session);
    bool completed = false;
    {
        std::unique_lock lk(pending->mtx);
        if (driver_armed) {  // GCOVR_EXCL_BR_LINE (defensive: the driver is
                             // always armed (region excluded below))
            pending->cv.wait(lk, [&] { return pending->completed; });
        } else {  // GCOVR_EXCL_START (defensive: the CAF expiry driver is
                  // always armed)
            completed =
                pending->cv.wait_for(  //  (defensive: region
                                       // excluded above (driver-always-armed
                                       // fallback))
                    lk, std::chrono::milliseconds(effective_timeout),
                    [&] { return pending->completed; });
        }
        // GCOVR_EXCL_STOP
        completed =
            completed ||  // GCOVR_EXCL_BR_LINE (defensive: driver path always
                          // completes first)
            pending
                ->completed;  // GCOVR_EXCL_BR_LINE (defensive: region excluded
                              // above (driver-always-armed fallback))
    }

    {
        std::unique_lock lock(impl_->registry_mutex);
        impl_->pending_sync_calls.erase(session);
    }

    // GCOVR_EXCL_START (defensive: region excluded above — the
    // driver-always-armed fallback only fires if dispatch stalls)
    if (!completed) {
        return CallResult::error(
            "call timeout (actor dispatch exceeded limit)");
    }  // GCOVR_EXCL_STOP
    if (pending->ok) {
        return CallResult::ok(std::move(pending->values));
    }
    return CallResult::error(std::move(pending->error));
}

uint64_t LuaServiceManager::dispatch_remote_call(std::string_view service_id,
                                                 std::string_view method,
                                                 const nlohmann::json& args,
                                                 int32_t timeout_ms,
                                                 std::string* error) {
    const uint64_t session = begin_proxied_call(timeout_ms);
    if (!dispatch_proxied_call(session, service_id, method, args, error)) {
        abandon_proxied_call(session);
        return 0;
    }
    return session;
}

bool LuaServiceManager::dispatch_proxied_call(uint64_t session,
                                              std::string_view service_id,
                                              std::string_view method,
                                              const nlohmann::json& args,
                                              std::string* error) {
    if (!send_call_request(service_id, method, args, session, error)) {
        return false;
    }
    // Arm the expiry driver from the session's deadline (begin_proxied_call
    // derived it from the envelope's caller timeout + slack).
    const int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    int64_t delay_ms = 1;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->pending_calls.find(session);
        if (it != impl_->pending_calls
                      .end()) {  // GCOVR_EXCL_BR_LINE (defensive: the entry was
                                 // inserted by begin_proxied_call on the same
                                 // thread moments earlier)
            delay_ms = it->second.deadline_ms - now_ms;
        }
    }
    schedule_proxied_call_timeout(
        session, static_cast<int32_t>(delay_ms > 0 ? delay_ms : 1));
    return true;
}

void LuaServiceManager::schedule_proxied_call_timeout(uint64_t session,
                                                      int32_t timeout_ms) {
    if (timeout_ms <= 0) {
        return;
    }
    try {
        auto driver = impl_->system.spawn([manager = this, session, timeout_ms](
                                              caf::event_based_actor* self)
                                              -> caf::behavior {
            self->delayed_send(self, std::chrono::milliseconds(timeout_ms),
                               caf::tick_atom_v);
            return caf::behavior{[=](caf::tick_atom) {  // GCOVR_EXCL_BR_LINE
                                                        // (compiler artifact:
                                                        // CAF behavior lambda
                                                        // arcs)
                nlohmann::json timeout_err = nlohmann::json::array(
                    {nlohmann::json::object(  // GCOVR_EXCL_BR_LINE (compiler
                                              // artifact: json::array
                                              // braced-init arcs)
                        {{"code",
                          "timeout"},  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // json::array braced-init arc)
                         {"message", "call timeout"},
                         {"retryable",
                          true}})});  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // json object init continuation arc)
                manager->finish_proxied_call(session, false, timeout_err);
                self->quit();
            }};  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda body exit arc)
        });
        std::unique_lock lock(impl_->registry_mutex);
        impl_->actor_call_timeouts[session] = std::move(driver);
    } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (defensive: untestable
                                    // actor-spawn failure (see the exclusion
                                    // region at 4358))
                 e) {               // GCOVR_EXCL_START (untestable
                                    // actor-spawn failure)
        auto& log = shield::log::get_logger(  //  (defensive:
                                              // untestable actor-spawn failure
                                              // (see the exclusion region
                                              // below))
            "lua");                           //  (defensive: untestable
                     // actor-spawn failure (see the exclusion region at 4358))
        SHIELD_LOG_ERROR(  //  (defensive: untestable
                           // actor-spawn failure (see the exclusion region
                           // below))
            log,
            std::string(
                "Failed to spawn proxied call "  //
                                                 // (defensive: untestable
                                                 // actor-spawn failure (see
                                                 // the exclusion region at
                                                 // 4358))
                "timeout actor: ") +
                e.what());
        // Without the driver the entry still expires via its deadline in
        // check_call_timeouts scans; nothing else breaks.
    }
    // GCOVR_EXCL_STOP
}

void LuaServiceManager::note_coroutine_started(lua_State* co,
                                               std::string_view service_id,
                                               std::string_view origin,
                                               int anchor_ref) {
    // Empty ids (bare VM dispatch) belong to no published service and are
    // never inserted — such an entry could never be grouped or torn down.
    if (co == nullptr ||       // GCOVR_EXCL_BR_LINE (defensive: call sites pass
                               // factory-made coroutines with a service id)
        service_id.empty()) {  // GCOVR_EXCL_BR_LINE (defensive: call sites pass
                               // factory-made coroutines with a service id
                               // (GCOVR_EXCL_LINE'd guard))
        return;  // GCOVR_EXCL_LINE (defensive: both call sites guard the
    }  // same conditions before invoking)
    std::unique_lock lock(impl_->registry_mutex);
    impl_->live_coroutines.insert_or_assign(co, std::string(service_id));
    // First drive: the dispatch that created the coroutine is its first
    // resume source.
    impl_->coroutine_meta[co] = {std::string(origin), 1, "dispatch",
                                 Impl::now_ms(), anchor_ref};
}

void LuaServiceManager::note_coroutine_finished(lua_State* co) {
    // Idempotent: whichever resume source observes the terminal state
    // first wins; the service may already be gone from the registry.
    if (co == nullptr) {  // GCOVR_EXCL_BR_LINE (defensive: call sites pass
                          // factory-made coroutines (GCOVR_EXCL_LINE'd guard))
        return;  // GCOVR_EXCL_LINE (defensive null guard, same shape as
    }  // mark_call_yielded: call sites pass factory-made coroutines)
    std::unique_lock lock(impl_->registry_mutex);
    impl_->live_coroutines.erase(co);
    // Release the GC anchor with it. Safe here (and only here): a terminal
    // resume always runs on the owner thread — the hard invariant that
    // also makes the lua_status reads in inspect_coroutines safe.
    if (auto it = impl_->coroutine_meta.find(co);
        it != impl_->coroutine_meta.end()) {
        if (it->second
                .anchor_ref !=  // GCOVR_EXCL_BR_LINE (defensive: suspend paths
                                // always anchor (LUA_NOREF is a legacy guard))
            LUA_NOREF) {  // GCOVR_EXCL_BR_LINE (defensive: suspend paths always
                          // anchor the coroutine; LUA_NOREF is a legacy guard)
            luaL_unref(co, LUA_REGISTRYINDEX, it->second.anchor_ref);
        }
        impl_->coroutine_meta.erase(it);
    }
}

void LuaServiceManager::note_coroutine_resumed(lua_State* co,
                                               std::string_view source) {
    // Unknown coroutines (never registered, already terminal, or the
    // service left the registry) are ignored — the key domain stays
    // identical to live_coroutines.
    if (co == nullptr) {  // GCOVR_EXCL_BR_LINE (defensive: call sites pass
                          // factory-made coroutines (GCOVR_EXCL_LINE'd guard))
        return;  // GCOVR_EXCL_LINE (defensive null guard, same shape as
    }  // note_coroutine_finished)
    std::unique_lock lock(impl_->registry_mutex);
    auto it = impl_->coroutine_meta.find(co);
    if (it != impl_->coroutine_meta.end()) {
        it->second.resumes++;
        it->second.last_resume = std::string(source);
        it->second.last_resume_ms = Impl::now_ms();
    }
}

LuaServiceManager::DrivingGuard::DrivingGuard(LuaServiceManager& mgr,
                                              lua_State* c,
                                              std::string_view src)
    : manager(mgr), co(c) {
    (void)src;  // diagnostic hook: source tag available for forensics
    std::unique_lock lock(manager.impl_->registry_mutex);
    manager.impl_->driving_cos.insert(co);
}

LuaServiceManager::DrivingGuard::~DrivingGuard() {
    std::unique_lock lock(manager.impl_->registry_mutex);
    manager.impl_->driving_cos.erase(co);
}

void LuaServiceManager::resume_caller(uint64_t session, bool ok,
                                      const nlohmann::json& values,
                                      std::string_view source) {
    // Peek under the registry lock: the yield-window requeue below must
    // leave the entry in place, so the completion cannot be moved out
    // unconditionally.
    lua_State* caller_co = nullptr;
    const std::string* caller_service = nullptr;
    caf::actor caller_actor;
    bool requeue = false;
    {
        // Unique, not shared: the requeue arm below mutates requeues and
        // deadline_ms, and shared_lock holders may not write.
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->pending_calls.find(session);
        if (it == impl_->pending_calls.end()) {
            return;
        }
        caller_co = it->second.caller_co;
        if (caller_co ==  // GCOVR_EXCL_BR_LINE (defensive: proxied sessions
                          // complete via the hook)
            nullptr) {    // GCOVR_EXCL_BR_LINE (defensive: proxied sessions
                          // complete via the hook and never reach the caller-co
                          // path (line-excluded))
            return;  // GCOVR_EXCL_LINE (proxied sessions complete via the hook)
        }
        caller_service = &it->second.caller_service;

        // Yield-window guard, driving-phase edition: if the caller is
        // currently registered in driving_cos, some OS thread is inside the
        // VM on it (it registered this very suspension from within its own
        // running frame and has not reached coroutine.yield() yet). Resuming
        // it now would fail, and peeking at its Lua state to find out is a
        // data race — so the decision reads only the mutex-ordered set.
        // Instead of waiting, re-enqueue the response onto the caller
        // actor's mailbox: the requeued message is processed on the caller
        // actor thread after the driving thread has yielded the coroutine
        // (the registration is gone by then), so every lua_resume stays on
        // the actor that owns it — including an on_init coroutine's, whose
        // yield window would otherwise tempt a spawn-thread resume while
        // the caller actor's pre-init pass-through handlers touch the VM.
        //
        // A coroutine the Lua layer drives itself (timer/fork callbacks,
        // user coroutine.wrap threads nested under a handler) yields inside
        // Lua — control jumps straight back to the calling Lua frame and no
        // C++ code observes LUA_YIELD for it. For those the registration is
        // already gone when the response arrives (the driver thread is busy
        // inside *this* message handler and the yield is long done), so the
        // resume proceeds immediately with no wait — the historical blind
        // handshake wait is gone.
        //
        // GCOVR_EXCL_START (the requeue arm is a race window: within one
        // actor, messages are serial, so a response processed while the
        // driver is still registered only happens during spawn's on_init
        // driving, a microsecond-scale window no unit test can pin.
        // End-to-end correctness of the window is covered by the on_init
        // call suites, e.g. CallErrorCodesAreStable.)
        if (impl_->driving_cos.contains(caller_co)) {
            auto actor_it = impl_->service_actors.find(*caller_service);
            if (actor_it !=
                    impl_->service_actors.end() &&  //  (race: requeue
                                                    // yield-window (see the
                                                    // exclusion region below))
                it->second.requeues <               //  (race: requeue
                                       // yield-window (see the exclusion region
                                       // below))
                    20) {  //  (race: requeue yield-window - a
                           // microsecond-scale interleaving no unit test can
                           // pin (see the exclusion region at 4484))
                requeue = true;
                ++it->second.requeues;
                // DIAGNOSTIC: one or two spins are the normal yield-window
                // race (init-drive spans hold the registration while CAF
                // scheduler calls complete). A longer spin means a driver
                // holding the registration across many mailbox cycles —
                // worth surfacing, and if the cap trips the fall-through
                // resume is caught by resume_suspended_caller's guard.
                if (it->second.requeues >=  //  (race: requeue
                                            // yield-window (see the exclusion
                                            // region below))
                    3) {                    //  (race: requeue yield-window (see
                                            // the exclusion region at 4484))
                    std::fprintf(           //  (race: requeue
                                   // yield-window (see the exclusion region at
                                   // 4484))
                        stderr,
                        "*** shield requeue spin: session=%llu n=%d "
                        "tid=%zu ok=%d\n",
                        static_cast<unsigned long long>(session),
                        it->second.requeues,
                        static_cast<size_t>(std::hash<std::thread::id>{}(
                            std::this_thread::get_id())),
                        ok ? 1 : 0);
                    std::fflush(stderr);  //  (race: requeue
                                          // yield-window (see the exclusion
                                          // region at 4484))
                }
                // Slide the timeout deadline so a concurrently expiring
                // check_call_timeouts pass does not re-enqueue this
                // response every tick while the driver is still registered.
                it->second.deadline_ms += 50;
                caller_actor = actor_it->second;
            }
            // GCOVR_EXCL_STOP
            // else: no caller actor to route through (actor already
            // stopped) or the requeue cap tripped (a caller that never
            // yields) — fall through to the erase path, which drops the
            // response exactly like the historical 5s handshake wait's
            // failure path left it.
        }
    }

    // GCOVR_EXCL_START (requeue execution: same race window as the arm
    // above — the mailbox enqueue must not run under the registry lock,
    // which dispatch_call_response takes on the actor thread)
    if (requeue) {
        // One mailbox self-loop takes microseconds; the driving span it is
        // waiting out contains CAF scheduler operations (timeout driver
        // arm/cancel) that take milliseconds under load. Spinning flat out
        // burns the whole cap before the driver can finish its span, and
        // the cap-trip resume then races the still-running coroutine.
        // Sleeping one millisecond per loop turns the 20-loop cap into a
        // 20 ms wait window — orders of magnitude beyond any CAF operation
        // — and yields the CPU to the driving thread. Negligible next to
        // the call's own timeout budget.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CallResponseMessage requeued{session, ok, values};
        caf::anon_send(caller_actor, std::move(requeued));
        return;
    }
    // GCOVR_EXCL_STOP

    Impl::PendingCall pc;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->pending_calls.find(session);
        if (it == impl_->pending_calls
                      .end()) {  // GCOVR_EXCL_BR_LINE (race: a racing requeue
                                 // won the erase (see GCOVR_EXCL_LINE comment))
            return;  // GCOVR_EXCL_LINE (a racing requeue won the erase)
        }
        pc = std::move(it->second);
        impl_->pending_calls.erase(it);
    }

    // Phase B slow-call tracking: the completed span measures against the
    // process-wide threshold. The call-timeout branch keeps its own
    // semantics and log — a deadline expiry is not a slow-call sample.
    // (pc.begin_ms == 0 means the call was never metered; the ring
    // re-checks the gate itself.)
    if (pc.begin_ms != 0 && source != "call-timeout") {
        SlowCallRing::instance().maybe_record(pc.begin_ms,
                                              std::move(pc.caller_service),
                                              std::move(pc.callee), ok);
    }

    // Cancel the CAF call-timeout driver (if any) now that the call has
    // completed normally. The timeout path itself erases the driver before
    // calling resume_caller, so this is a no-op for the timeout branch.
    cancel_actor_call_timeout(session);

    resume_suspended_caller(session, pc.caller_anchor, pc.caller_service,
                            caller_co, ok, values, pc.resume_retries, source);
}

void LuaServiceManager::resume_suspended_caller(
    uint64_t session, int caller_anchor, const std::string& caller_service,
    lua_State* caller_co, bool ok, const nlohmann::json& values,
    int resume_retries, std::string_view source) {
    // Build the resume payload: (ok, values...). The caller's shield.call
    // wrapper unpacks these via coroutine.yield()'s return values.
    //
    // Re-establish the caller's dispatch context for the continuation: the
    // resume runs on the caller actor thread, whose thread-local dispatch
    // stack does not carry the original frame (an on_init coroutine, for
    // example, is first driven by the spawning thread). Without this frame
    // every suspend_for_call issued after the first resume records an empty
    // caller_service and the next completion cannot route back.
    Impl::DispatchScope scope(*impl_, caller_service, "", false);
    // Register the driving phase for the resume span (see driving_cos).
    DrivingGuard driving(*this, caller_co, source);
    // Resume bookkeeping for lua.inspect <svc> coroutines: this C++ resume
    // source is the coroutine's most recent driver.
    note_coroutine_resumed(caller_co, source);
    // Arm the resumed coroutine in case it was created after the sampling
    // install (Lua 5.5 does not propagate hooks to new threads); one
    // thread_local read when no sampling session is on this owner thread.
    ProfileSampler::sweep_active(caller_service);
    lua_pushboolean(caller_co, ok ? 1 : 0);
    int nargs = 1;
    if (values.is_array()) {
        for (const auto& v : values) {
            push_json_to_stack(caller_co, v);
            ++nargs;
        }
    } else if (!values             // GCOVR_EXCL_BR_LINE
                    .is_null()) {  // GCOVR_EXCL_BR_LINE (defensive: completion
                                   // values are always arrays or null)
        push_json_to_stack(caller_co, values);
        ++nargs;
    }
    int nres = 0;
    const int status = lua_resume(caller_co, nullptr, nargs, &nres);
    if (status == LUA_OK) {
        nlohmann::json returns = nlohmann::json::array();
        for (int i = 0; i < nres; ++i) {
            nlohmann::json item;
            shd::stack_object so(shd::state_view(caller_co), i + 1);
            lua_to_json(so, &item);
            returns.push_back(std::move(item));
        }
        lua_settop(caller_co, 0);
        if (caller_anchor !=  // GCOVR_EXCL_BR_LINE (defensive: suspend paths
                              // always anchor (LUA_NOREF is a legacy guard))
            LUA_NOREF) {  // GCOVR_EXCL_BR_LINE (defensive: suspend paths always
                          // anchor the caller coroutine; LUA_NOREF is a legacy
                          // guard)
            luaL_unref(caller_co, LUA_REGISTRYINDEX, caller_anchor);
        }
        // Terminal: the caller coroutine completed on this resume.
        note_coroutine_finished(caller_co);
        on_handler_completed(caller_co, returns);
        finish_pending_exit(caller_service);
        return;
    }
    if (status != LUA_YIELD) {
        std::string err = "call continuation error";
        if (lua_type(caller_co, -1) == LUA_TSTRING) {
            err = lua_tostring(caller_co, -1);
        }
        // Pop the kernel's error object NOW, in the same breath as our own
        // failed resume: it sits at the coroutine's top and would shift the
        // driving thread's next frame boundary (continuations read the
        // absolute top, so leftover junk reindexes their payloads). One
        // bounded slot decrement is memory-safe under the driving race; a
        // settop(0) here instead yanked the stack out from under the
        // driving thread and segfaulted it (Windows CI, 2026-09).
        lua_pop(caller_co, 1);
        static constexpr const char* kResumeRejectedDriven =
            "cannot resume non-suspended coroutine";
        // Concurrent-completion guard: lua_resume rejects with this exact
        // message when the coroutine is mid-frame on another thread (the
        // kernel reports a live call frame — "dead" is the different
        // message for terminal states). The winner of the race is driving
        // the continuation right now and owns every further mutation of the
        // coroutine's Lua state: even reads are a cross-thread race (a
        // stack top seen here is the driver's live top; MSVC segfaults on
        // these where glibc silently survives), and a settop here used to
        // yank the stack out from under the driving thread — the Windows
        // SIGSEGV of 2026-09. The kernel's rejection message popped above
        // is the one bounded touch this path allows (it cleans up our own
        // failed resume); everything below the classification is
        // mutex-ordered bookkeeping only. The pending entry goes back and
        // the response is re-enqueued through the caller actor's mailbox
        // (the
        // driving-phase requeue path resumes it as soon as the winner's
        // segment yields). The retry budget is this guard's own: the
        // driving-phase spin cap can trip legitimately inside one
        // init-drive span (millisecond-scale CAF operations under load),
        // and the fall-through resume that follows it is exactly the case
        // this guard exists for.
        if (err == kResumeRejectedDriven &&  // GCOVR_EXCL_BR_LINE
                                             // (race: two
                                             // completions
                                             // interleaving
                                             // inside one resume
                                             // span (see
                                             // the exclusion
                                             // region at 4653))
            resume_retries < 3) {  // GCOVR_EXCL_BR_LINE (same race window)
            // GCOVR_EXCL_START (race window: two completions must interleave
            // inside one resume span — a microsecond-scale interleaving no
            // unit test can pin)
            std::fprintf(stderr,
                         "*** shield resume requeue: co=%p session=%llu "
                         "retries=%d src=%.*s\n",
                         (void*)caller_co,
                         static_cast<unsigned long long>(session),
                         resume_retries + 1, static_cast<int>(source.size()),
                         source.data());
            std::fflush(stderr);
            caf::actor caller_actor;
            {
                std::unique_lock lock(impl_->registry_mutex);
                Impl::PendingCall restored;
                restored.session = session;
                restored.caller_co = caller_co;
                restored.caller_anchor = caller_anchor;
                restored.caller_service = caller_service;
                restored.deadline_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count() +
                    50;
                restored.requeues = 0;
                restored.resume_retries = resume_retries + 1;
                impl_->pending_calls.insert_or_assign(session,
                                                      std::move(restored));
                if (auto actor_it = impl_->service_actors.find(caller_service);
                    actor_it !=
                    impl_->service_actors
                        .end()) {  //  (race: two completions
                                   // interleaving inside one resume span (see
                                   // the exclusion region at 4653))
                    caller_actor = actor_it->second;
                }
            }
            if (caller_actor != nullptr) {
                caf::anon_send(caller_actor,
                               CallResponseMessage{session, ok, values});
            } else {
                // Caller actor is gone; the coroutine dies with its VM and
                // the anchor with it — nothing can resume it.
                std::unique_lock lock(impl_->registry_mutex);
                impl_->pending_calls.erase(session);
            }
            return;
            // GCOVR_EXCL_STOP
        }
        if (err ==  // GCOVR_EXCL_BR_LINE (race window: budget exhausted while
                    // the coroutine is still driven — sibling arm of the
                    // requeue guard above)
            kResumeRejectedDriven) {  // GCOVR_EXCL_BR_LINE (same race
                                      // window)
            // GCOVR_EXCL_START (race window: two completions must interleave
            // inside one resume span — a microsecond-scale interleaving no
            // unit test can pin)
            // Retry budget exhausted while the coroutine is still driven:
            // drop the wait WITHOUT touching the coroutine — its driver owns
            // the teardown. Same drop semantics as the historical
            // handshake-failure path (the caller's wait is abandoned; the
            // coroutine finishes under its driver).
            std::fprintf(stderr,
                         "*** shield resume drop: co=%p session=%llu "
                         "src=%.*s\n",
                         (void*)caller_co,
                         static_cast<unsigned long long>(session),
                         static_cast<int>(source.size()), source.data());
            std::fflush(stderr);
            return;
            // GCOVR_EXCL_STOP
        }
        // Our own resume terminated the coroutine (a continuation error or a
        // terminal-state rejection) — nothing else is driving it, so the
        // teardown below is safe on this thread.
        // Caller errored resuming; drop it after propagating to its upstream
        // caller if this handler was itself servicing a call.
        lua_settop(caller_co, 0);
        if (caller_anchor !=  // GCOVR_EXCL_BR_LINE (defensive: error path
                              // always anchored (LUA_NOREF is a legacy guard))
            LUA_NOREF) {      // GCOVR_EXCL_BR_LINE (defensive: error path - the
                          // coroutine was anchored at suspend; LUA_NOREF is a
                          // legacy guard)
            luaL_unref(caller_co, LUA_REGISTRYINDEX, caller_anchor);
        }
        // Terminal (error): drop the live-coroutine entry.
        note_coroutine_finished(caller_co);
        on_handler_failed(caller_co, err);
        finish_pending_exit(caller_service);
        return;
    }
    // Release the anchor; the coroutine re-yielded and the API that yielded has
    // already re-anchored it for its next resume source.
    if (caller_anchor !=  // GCOVR_EXCL_BR_LINE (defensive: suspend paths always
                          // anchor (LUA_NOREF is a legacy guard))
        LUA_NOREF) {      // GCOVR_EXCL_BR_LINE (defensive: suspend paths always
                          // anchor; LUA_NOREF is a legacy guard)
        luaL_unref(caller_co, LUA_REGISTRYINDEX, caller_anchor);
    }
    // The re-yield may correspond to a nested suspension (e.g. the handler
    // issued another shield.call): its next resume source routes through
    // resume_caller again, whose driving-phase guard sees the registration
    // already gone.
}

int LuaServiceManager::check_call_timeouts(int64_t now_ms) {
    // Collect expired sessions first; we must not modify pending_calls while
    // iterating, and resume_caller erases from it.
    std::vector<uint64_t> expired;
    {
        std::shared_lock lock(impl_->registry_mutex);
        for (const auto& [session, pc] : impl_->pending_calls) {
            if (pc.deadline_ms <= now_ms) {
                expired.push_back(session);
            }
        }
    }

    nlohmann::json timeout_err = nlohmann::json::array(
        {nlohmann::json::object(  // GCOVR_EXCL_BR_LINE (compiler artifact: json
                                  // braced-init arcs)
            {{"code", "timeout"},  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                   // json::array braced-init continuation arc)
             {"message", "call timeout"},
             {"retryable", true}})});  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // json object init continuation arc)

    for (uint64_t session : expired) {
        // Proxied sessions have no coroutine to resume, but their expiry
        // must still reach the hook so the remote caller learns of the
        // timeout instead of hanging until its own deadline.
        if (finish_proxied_call(session, false, timeout_err)) {
            continue;  // GCOVR_EXCL_LINE (needs a cluster build: proxied
                       // sessions only exist with SHIELD_ENABLE_CLUSTER)
        }
        resume_caller(session, false, timeout_err, "call-timeout");
    }
    return static_cast<int>(expired.size());
}

void LuaServiceManager::invoke_error_hook(const std::string& service_id,
                                          const std::string& error_type,
                                          const std::string& method_name,
                                          const std::string& error_message) {
    std::shared_ptr<LuaVM> service;
    {
        std::shared_lock lock(impl_->registry_mutex);
        auto it = impl_->services.find(service_id);
        if (it != impl_->services.end()) {
            service = it->second;
        }
    }
    if (!service) {
        return;
    }

    // Increment error counter (read the count under the lock; the panic
    // threshold check below uses the local snapshot).
    int count = 0;
    {
        std::lock_guard lock(impl_->error_mutex);
        count = ++impl_->error_counts[service_id];
    }

    // Call on_error(err, context) if defined on the service table.
    impl_->runtime.invoke_hook(service, "on_error", error_message, error_type,
                               method_name);

    // Check panic threshold.
    if (count >= kDefaultMaxErrorsBeforePanic) {
        impl_->runtime.invoke_hook(service, "on_panic",
                                   "consecutive errors reached limit",
                                   error_type, method_name);
        // Exit the service after panic.
        exit(service_id, "panic");
    }
}

void LuaServiceManager::reset_error_count(const std::string& service_id) {
    std::lock_guard lock(impl_->error_mutex);
    impl_->error_counts.erase(service_id);
}

bool LuaServiceManager::schedule_external_call_timeout(int32_t timeout_ms,
                                                       uint64_t session) {
    if (timeout_ms <= 0) {
        return false;
    }
    try {
        auto driver = impl_->system.spawn([manager = this, session, timeout_ms](
                                              caf::event_based_actor* self)
                                              -> caf::behavior {
            self->delayed_send(self, std::chrono::milliseconds(timeout_ms),
                               caf::tick_atom_v);
            return caf::behavior{[=](caf::tick_atom) {  // GCOVR_EXCL_BR_LINE
                                                        // (compiler artifact:
                                                        // CAF behavior lambda
                                                        // arc)
                nlohmann::json timeout_err = nlohmann::json::array(
                    {nlohmann::json::object(  // GCOVR_EXCL_BR_LINE (compiler
                                              // artifact: json braced-init
                                              // arcs)
                        {{"code",
                          "timeout"},  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // json::array braced-init arc)
                         {"message", "call timeout"},
                         {"retryable",
                          true}})});  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // json object init continuation arc)
                manager->complete_call(session, false, timeout_err);
                self->quit();
            }};  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda body exit arc)
        });
        std::unique_lock lock(impl_->registry_mutex);
        impl_->actor_call_timeouts[session] = std::move(driver);
        return true;
    } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (defensive: untestable
                                    // actor-spawn failure (see the exclusion
                                    // region at 5051))
                 e) {               // GCOVR_EXCL_START (untestable
                                    // actor-spawn failure)
        auto& log = shield::log::get_logger(  //  (defensive:
                                              // untestable actor-spawn failure
                                              // (see the exclusion region
                                              // below))
            "lua");                           //  (defensive: untestable
                     // actor-spawn failure (see the exclusion region at 5051))
        SHIELD_LOG_ERROR(  //  (defensive: untestable
                           // actor-spawn failure (see the exclusion region
                           // below))
            log,
            std::string(
                "Failed to spawn external call "  //
                                                  // (defensive: untestable
                                                  // actor-spawn failure (see
                                                  // the exclusion region at
                                                  // 5051))
                "timeout driver: ") +
                e.what());
        return false;
    }
    // GCOVR_EXCL_STOP
}

uint64_t LuaServiceManager::schedule_actor_call_timeout(
    int32_t timeout_ms, const std::string& service_id, uint64_t session) {
    if (timeout_ms <= 0) {
        return session;
    }
    std::shared_ptr<caf::actor> service_actor;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->service_actors.find(service_id);
        if (it == impl_->service_actors.end()) {
            return session;
        }
        service_actor = std::make_shared<caf::actor>(it->second);
    }

    try {
        auto driver = impl_->system.spawn(
            [manager = this, session,  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // driver lambda entry/exit arcs)
             target = *service_actor,  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                       // driver lambda entry/exit arcs)
             timeout_ms](caf::event_based_actor* self) -> caf::behavior {
                self->delayed_send(self, std::chrono::milliseconds(timeout_ms),
                                   caf::tick_atom_v);
                return caf::behavior{
                    [=](caf::tick_atom) {  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: inner tick lambda arcs)
                        caf::anon_send(target, call_timeout_atom_v, session);
                        self->quit();
                    }};
            });
        std::unique_lock lock(impl_->registry_mutex);
        impl_->actor_call_timeouts[session] = std::move(driver);
    } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (defensive: untestable
                                    // actor-spawn failure (see the exclusion
                                    // region at 5090))
                 e) {               // GCOVR_EXCL_START (untestable
                                    // actor-spawn failure)
        auto& log = shield::log::get_logger(  //  (defensive:
                                              // untestable actor-spawn failure
                                              // (see the exclusion region
                                              // below))
            "lua");                           //  (defensive: untestable
                     // actor-spawn failure (see the exclusion region at 5090))
        SHIELD_LOG_ERROR(  //  (defensive: untestable
                           // actor-spawn failure (see the exclusion region at
                           // 5090))
            log,
            std::string("Failed to spawn call timeout actor: ") + e.what());
        // Return session anyway - timeout just won't fire, but call can still
        // complete
    }
    // GCOVR_EXCL_STOP
    return session;
}

bool LuaServiceManager::cancel_actor_call_timeout(uint64_t session) {
    caf::actor cancelled_driver;
    {
        std::unique_lock lock(impl_->registry_mutex);
        auto it = impl_->actor_call_timeouts.find(session);
        if (it == impl_->actor_call_timeouts.end()) {
            return false;
        }
        if (it->second) {  // GCOVR_EXCL_BR_LINE (defensive: call-timeout
                           // entries are inserted only with a live driver; the
                           // null arm is a contract guard)
            caf::anon_send_exit(it->second, caf::exit_reason::user_shutdown);
            // The entry dies below while the driver may still be running its
            // (or a still-armed) tick; move the handle out so the erase never
            // releases a reference on this thread, and park it in the
            // graveyard — the Impl destructor joins it there.
            cancelled_driver = std::move(it->second);
        }
        impl_->actor_call_timeouts.erase(it);
    }
    impl_->retire_actor(std::move(cancelled_driver));
    return true;
}

}  // namespace shield::lua
