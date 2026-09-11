// [SHIELD_LUA] Gateway actor implementation
#include "shield/lua/gateway_actor.hpp"

#include <caf/event_based_actor.hpp>
#include <nlohmann/json.hpp>
#include <utility>

#include "shield/core/service_message.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/lua_service.hpp"
#include "shield/transport/protocol.hpp"

namespace shield::lua {

namespace {

constexpr const char* kErrEpochExpired = "client_rpc.epoch_expired";

auto& gateway_log() { return shield::log::get_logger("lua"); }

/// Client reference marker for a freshly applied binding. gateway_address is
/// this gateway (the registry is per-listener, so a reference pointing at
/// another gateway can never hit this registry); profile id carries over.
ClientContextData fresh_context(const GatewayDeps& deps,
                                const ClientContextData& requested,
                                const net::SessionBinding& updated) {
    ClientContextData fresh = requested;
    fresh.gateway_address = deps.gateway_name;
    fresh.session_epoch = updated.epoch;
    fresh.player_id = updated.player_id;
    return fresh;
}  // GCOVR_EXCL_LINE (gcov clone artifact)

void fail_bind(GatewayDeps& deps, const ClientBindRequest& request) {
    ++deps.stats->binds_epoch_expired;
    SHIELD_LOG_WARNING(gateway_log(),
                       "client bind rejected: client_rpc.epoch_expired for "
                       "session " +
                           std::to_string(request.context.session_id));
    if (request.call_session != 0) {
        deps.manager->complete_call(request.call_session, false,
                                    nlohmann::json::array({nlohmann::json{
                                        {"code", kErrEpochExpired}}}));
    }
}

}  // namespace

// -- GatewaySessionRegistry ---------------------------------------------------

void GatewaySessionRegistry::add(const std::shared_ptr<net::Session>& session,
                                 net::SessionBinding initial) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[session->id()] = Entry{session, std::move(initial)};
}

void GatewaySessionRegistry::remove(net::SessionId session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(session_id);
}

std::shared_ptr<net::Session> GatewaySessionRegistry::find(
    net::SessionId session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(session_id);
    if (it == entries_.end()) {
        return nullptr;
    }
    auto session = it->second.session.lock();
    if (!session) {
        // Reap lazily: the session died without a disconnect callback.
        entries_.erase(it);
    }
    return session;
}

size_t GatewaySessionRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

// -- Validation entry points --------------------------------------------------

void handle_client_egress(GatewayDeps& deps, const ClientEgress& egress) {
    const ClientContextData& ctx = egress.context;
    auto session = deps.registry->find(ctx.session_id);
    if (session == nullptr) {
        ++deps.stats->egress_unknown_session;
        SHIELD_LOG_WARNING(gateway_log(),
                           "client egress rejected: unknown session " +
                               std::to_string(ctx.session_id));
        return;
    }

    const net::SessionBinding binding = session->binding();
    if (ctx.session_epoch != binding.epoch) {
        ++deps.stats->egress_stale_epoch;
        SHIELD_LOG_WARNING(gateway_log(),
                           "client egress rejected: stale epoch " +
                               std::to_string(ctx.session_epoch) +
                               " (current " + std::to_string(binding.epoch) +
                               ") for session " +
                               std::to_string(ctx.session_id));
        return;
    }
    if (ctx.player_id != binding.player_id) {
        ++deps.stats->egress_owner_mismatch;
        SHIELD_LOG_WARNING(gateway_log(),
                           "client egress rejected: player mismatch for "
                           "session " +
                               std::to_string(ctx.session_id));
        return;
    }

    const transport::RpcDescriptor* descriptor =
        deps.descriptors.find(egress.route_id);
    if (descriptor == nullptr) {
        ++deps.stats->egress_route_not_found;
        SHIELD_LOG_WARNING(gateway_log(),
                           "client egress rejected: unknown route " +
                               std::to_string(egress.route_id));
        return;
    }
    if (descriptor->direction == transport::RouteDirection::ClientToServer) {
        ++deps.stats->egress_direction_rejected;
        SHIELD_LOG_WARNING(gateway_log(), "client egress rejected: route " +
                                              std::to_string(egress.route_id) +
                                              " is client-to-server");
        return;
    }

    transport::DecodedBody body;
    body.route_id = egress.route_id;
    body.route_name = descriptor->name;
    if (egress.message.has_value()) {
        body.message = egress.message;
    } else {
        body.bytes = egress.body_bytes;
    }
    std::string error;
    if (!session->send_message(body, &error)) {
        ++deps.stats->egress_send_failed;
        SHIELD_LOG_WARNING(gateway_log(), "client egress dropped for session " +
                                              std::to_string(ctx.session_id) +
                                              ": " + error);
        return;
    }
    ++deps.stats->egress_accepted;
}

void handle_client_bind(GatewayDeps& deps, const ClientBindRequest& request) {
    auto session = deps.registry->find(request.context.session_id);
    if (session == nullptr) {
        fail_bind(deps, request);
        return;
    }

    net::SessionBinding updated;
    if (!session->apply_binding(request.target_service, request.player_id,
                                request.context.session_epoch, &updated)) {
        fail_bind(deps, request);
        return;
    }
    deps.registry->add(session, updated);
    ++deps.stats->binds_ok;

    if (request.call_session != 0) {
        deps.manager->complete_call(
            request.call_session, true,
            nlohmann::json::array(
                {fresh_context(deps, request.context, updated).to_json()}));
    }
    // [M3] A typed ClientControlMessage::Bound is delivered to the new
    // target service together with the ingress dispatch flip.
}

void handle_client_close(GatewayDeps& deps, const ClientCloseRequest& request) {
    ++deps.stats->close_requests;
    auto session = deps.registry->find(request.context.session_id);
    if (session == nullptr) {
        SHIELD_LOG_WARNING(gateway_log(),
                           "client close for unknown session " +
                               std::to_string(request.context.session_id));
        return;
    }
    // Invalidate first: outstanding references (old epochs) must fail from
    // this point on, even before the socket actually closes.
    session->apply_binding("", "", net::kAnyEpoch, nullptr);
    deps.registry->remove(request.context.session_id);
    session->close(request.reason.empty() ? net::CloseReason::KICKED
                                          : request.reason);
}

// -- Actor
// ---------------------------------------------------------------------

caf::actor spawn_gateway_actor(caf::actor_system& system, GatewayDeps deps) {
    auto state = std::make_shared<GatewayDeps>(std::move(deps));
    return system.spawn([state](caf::event_based_actor* self) {
        return caf::behavior{
            [state](ClientEgress egress) {
                handle_client_egress(*state, egress);
            },
            [state](ClientBindRequest request) {
                handle_client_bind(*state, request);
            },
            [state](ClientCloseRequest request) {
                handle_client_close(*state, request);
            },
        };
    });
}

}  // namespace shield::lua
