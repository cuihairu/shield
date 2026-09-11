// [SHIELD_LUA] Gateway actor: single-target session binding + client egress
//
// One gateway actor is spawned per TCP listener (see bootstrap.cpp). It owns
// the live-session registry for that listener and is the only component that
// mutates session bindings after connect:
//
//   - ClientEgress        validated server-to-client push (fire-and-forget)
//   - ClientBindRequest   compare-and-set binding replacement
//   (shield.client.bind)
//   - ClientCloseRequest  gateway-initiated close (shield.client.close)
//
// The session's binding (target service, player identity, epoch) has a
// single source of truth: the Session object itself. The registry only keeps
// a weak handle plus a connect-time snapshot for diagnostics, so egress and
// bind validation always read live state and can never drift from it.
//
// The validation entry points below are free functions on GatewayDeps (not
// closures inside the actor) so the coverage suites exercise the exact logic
// the actor runs without going through CAF.
#pragma once

#include <atomic>
#include <caf/actor.hpp>
#include <caf/actor_system.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "shield/core/service_message.hpp"
#include "shield/net/session.hpp"
#include "shield/transport/rpc_descriptor.hpp"

namespace shield::lua {

class LuaServiceManager;

/// Delivery/rejection counters for one gateway. Egress rejections are
/// dropped with a warning and counted; they are never queued or retried.
struct GatewayStats {
    std::atomic<uint64_t> egress_accepted{0};
    std::atomic<uint64_t> egress_unknown_session{0};  // no live session
    std::atomic<uint64_t> egress_stale_epoch{0};
    std::atomic<uint64_t> egress_owner_mismatch{0};
    std::atomic<uint64_t> egress_route_not_found{0};
    std::atomic<uint64_t> egress_direction_rejected{0};
    std::atomic<uint64_t> egress_send_failed{0};  // backpressure/closed
    std::atomic<uint64_t> binds_ok{0};
    std::atomic<uint64_t> binds_epoch_expired{0};
    std::atomic<uint64_t> close_requests{0};
};

/// Live-session table for one gateway listener. The bridge callbacks (net
/// threads) add/remove sessions; the gateway actor (CAF thread) looks them
/// up. Entries hold weak handles: a dead session is reaped lazily.
class GatewaySessionRegistry {
public:
    /// Registers (or refreshes the snapshot of) a live session.
    void add(const std::shared_ptr<net::Session>& session,
             net::SessionBinding initial);

    /// Idempotent remove (disconnect path and close request may race).
    void remove(net::SessionId session_id);

    /// Locked session handle; nullptr when unknown, and the entry is reaped
    /// when the weak handle has expired.
    std::shared_ptr<net::Session> find(net::SessionId session_id) const;

    size_t size() const;

private:
    struct Entry {
        std::weak_ptr<net::Session> session;
        net::SessionBinding snapshot;
    };
    mutable std::mutex mutex_;
    // mutable: find() reaps expired entries in place; every access holds
    // mutex_, so logical constness is preserved.
    mutable std::unordered_map<net::SessionId, Entry> entries_;
};

/// State captured by one gateway actor. Shared with the free validation
/// entry points; bootstrap keeps the registry/stats handles alive across
/// shutdown ordering.
struct GatewayDeps {
    std::string gateway_name;  // listener owner actor name
    std::shared_ptr<GatewaySessionRegistry> registry;
    std::shared_ptr<GatewayStats> stats;
    transport::RpcDescriptorTable descriptors;  // merged route metadata
    LuaServiceManager* manager = nullptr;       // bind call responses
};

// -- Validation entry points (also the actor behavior bodies) -----------------

/// Validates and enqueues a server-to-client push. Gate order: registry hit,
/// epoch equality, owner player_id match, descriptor route found, direction
/// allows server-to-client, then session->send_message (backpressure counts
/// as egress_send_failed).
void handle_client_egress(GatewayDeps& deps, const ClientEgress& egress);

/// Compare-and-set binding replacement. On success the caller (if any) gets
/// the fresh client reference marker via complete_call; on any failure the
/// response carries {"code": "client_rpc.epoch_expired"}.
void handle_client_bind(GatewayDeps& deps, const ClientBindRequest& request);

/// Invalidates the current binding (epoch bump), drops the registry entry,
/// and closes the socket. An unknown session is a no-op with a warning.
void handle_client_close(GatewayDeps& deps, const ClientCloseRequest& request);

/// Spawns one gateway actor on @p system carrying @p deps.
caf::actor spawn_gateway_actor(caf::actor_system& system, GatewayDeps deps);

}  // namespace shield::lua
