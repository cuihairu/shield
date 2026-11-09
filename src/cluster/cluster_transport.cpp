// [SHIELD_CLUSTER] ClusterTransport — facade lifecycle: CAF type
// registration, the constructor, publish/dial start, teardown stop
// and the runtime global accessor for admin surfaces.
// Split out of cluster_transport.cpp.
#include "shield/cluster/cluster_transport.hpp"

#include <algorithm>
#include <atomic>
#include <caf/actor_system.hpp>
#include <caf/error.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/io/middleman.hpp>
#include <caf/io/middleman_actor.hpp>
#include <caf/send.hpp>
#include <caf/stateful_actor.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cluster_transport_impl.hpp"
#include "shield/base/time.hpp"
#include "shield/cluster/cluster_auth.hpp"
#include "shield/cluster/cluster_messages.hpp"
#include "shield/log/logger.hpp"

namespace shield::cluster {

void init_cluster_caf_types() {
    caf::init_global_meta_objects<caf::id_block::shield_cluster>();
}

namespace {
ClusterTransport* g_cluster_transport = nullptr;
}

ClusterTransport::ClusterTransport(caf::actor_system& system,
                                   ClusterManager& manager,
                                   const ClusterConfig& config)
    : impl_(std::make_unique<Impl>()) {
    impl_->system = &system;
    impl_->manager = &manager;
    impl_->config = config;
}

ClusterTransport::~ClusterTransport() { stop(); }

bool ClusterTransport::start(uint16_t* bound_port, std::string& error) {
    if (impl_->running) {
        if (bound_port) *bound_port = impl_->bound_port;
        return true;
    }
    auto& log = shield::log::get_logger("cluster");

    if (impl_->config.auth.enabled &&
        (impl_->config.auth.token.size() < 32 ||
         impl_->config.auth.token.size() > 4096 ||
         impl_->config.auth.time_window_ms < 1 ||
         impl_->config.auth.time_window_ms > 300000)) {
        error = "invalid cluster.auth token length or time_window_ms";
        return false;
    }

    // NOTE: init_cluster_caf_types() must have been called before the actor
    // system was constructed (CAF requirement), not here.

    auto actor = impl_->system->spawn(transport_loop, impl_->manager,
                                      impl_->config, impl_->side);

    // Publish on the configured listen address. "0.0.0.0" (or an empty host
    // part) publishes on all interfaces; port 0 lets the OS pick.
    auto colon = impl_->config.listen_address.rfind(':');
    std::string host = colon == std::string::npos
                           ? impl_->config.listen_address
                           : impl_->config.listen_address.substr(0, colon);
    uint16_t port = 0;
    if (colon != std::string::npos &&
        colon + 1 < impl_->config.listen_address.size()) {
        port = static_cast<uint16_t>(
            std::stoi(impl_->config.listen_address.substr(colon + 1)));
    }
    const char* bind_host =
        (host.empty() || host == "0.0.0.0" ||
         host == "*")  // GCOVR_EXCL_BR_LINE (defensive: tests always configure
                       // a concrete listen host)
            ? nullptr
            : host.c_str();
    auto published = impl_->system->middleman().publish(actor, port, bind_host);
    if (!published) {
        error = "cluster listen failed on " + impl_->config.listen_address +
                ": " + caf::to_string(published.error());
        SHIELD_LOG_ERROR(log, error);
        caf::anon_send_exit(actor, caf::exit_reason::user_shutdown);
        return false;
    }
    impl_->bound_port = *published;
    impl_->running = true;

    {
        std::unique_lock lock(impl_->side->peers_mutex);
        impl_->side->outbound_actor = actor;
    }
    if (!impl_->config.auth.enabled)
        SHIELD_LOG_WARNING(
            log,
            "Cluster authentication disabled: configure cluster.auth token "
            "PSK; exposed ports trust self-reported identities");

    // Kick both loops.
    caf::anon_send(actor, connect_tick_atom_v);
    caf::anon_send(actor, hb_tick_atom_v);

    if (bound_port) *bound_port = impl_->bound_port;
    return true;
}

void ClusterTransport::stop() {
    if (!impl_->running) return;
    impl_->running = false;

    auto& log = shield::log::get_logger("cluster");
    SHIELD_LOG_INFO(log, "Cluster transport stopping");

    {
        std::unique_lock lock(impl_->side->peers_mutex);
        if (impl_->side->outbound_actor) {
            impl_->system->middleman().unpublish(impl_->side->outbound_actor);
            caf::anon_send_exit(impl_->side->outbound_actor,
                                caf::exit_reason::user_shutdown);
            impl_->side->outbound_actor = nullptr;
        }
    }
    impl_->bound_port = 0;

    // Drop data-plane state so later complete_proxied_call / send_envelope
    // calls fail honestly instead of racing the dying actor. Bridges are
    // cleared too: a straggler envelope in the actor's mailbox must not
    // dispatch into a service manager that shutdown is already releasing.
    {
        std::unique_lock lock(impl_->side->peers_mutex);
        impl_->side->peers_by_node.clear();
        impl_->side->inbound_peer_senders.clear();
        impl_->side->outbound_actor = nullptr;
    }
    {
        std::lock_guard lock(impl_->side->data_mutex);
        impl_->side->proxied_calls.clear();
        impl_->side->outbound_sessions.clear();
        impl_->side->bridges = {};
    }
}

ClusterTransport* global_cluster_transport() { return g_cluster_transport; }

void set_global_cluster_transport(ClusterTransport* transport) {
    g_cluster_transport = transport;
}
}  // namespace shield::cluster
