#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "caf/actor_system.hpp"
#include "caf/send.hpp"
#include "shield/actor/actor_registry.hpp"
#include "shield/core/service.hpp"
#include "shield/discovery/service_discovery.hpp"

namespace shield::actor {

/// @brief Configuration for distributed actor system
struct DistributedActorConfig {
    std::string node_id;                          // Unique node identifier
    std::string cluster_name = "shield_cluster";  // Cluster name for grouping
    uint16_t actor_port = 0;  // Port for actor communication (0 = auto)
    std::chrono::seconds heartbeat_interval{30};  // Heartbeat interval
    std::chrono::seconds discovery_interval{60};  // Discovery refresh interval
    bool auto_discovery = true;       // Enable automatic actor discovery
    size_t max_remote_actors = 1000;  // Maximum cached remote actors
};

/// @brief Event types for distributed actor system
enum class ActorSystemEvent {
    NODE_JOINED,       // New node joined the cluster
    NODE_LEFT,         // Node left the cluster
    ACTOR_DISCOVERED,  // New remote actor discovered
    ACTOR_LOST,        // Remote actor became unavailable
    CLUSTER_CHANGED    // Cluster topology changed
};

/// @brief Event data for actor system events
struct ActorSystemEventData {
    ActorSystemEvent event_type;
    std::string node_id;
    std::string actor_name;
    ActorType actor_type;
    std::chrono::steady_clock::time_point timestamp;
    std::map<std::string, std::string> metadata;
};

/// @brief Distributed actor system manager
/// Provides high-level interface for managing distributed CAF actors
class DistributedActorSystem : public core::Service {
public:
    /// @brief Event callback type
    using EventCallback = std::function<void(const ActorSystemEventData &)>;

    /// @brief Constructor
    /// @param actor_system CAF actor system instance
    /// @param discovery_service Service discovery implementation
    /// @param config Configuration for the distributed system
    explicit DistributedActorSystem(
        caf::actor_system &actor_system,
        std::shared_ptr<discovery::IServiceDiscovery> discovery_service,
        const DistributedActorConfig &config);

    /// @brief Constructor for use with Starter system
    /// @param name Service name
    /// @param config Configuration for the distributed system
    explicit DistributedActorSystem(const std::string &name,
                                    const DistributedActorConfig &config);

    /// @brief Destructor
    ~DistributedActorSystem();

    // Service interface implementation
    void on_init(core::ApplicationContext &ctx) override;
    void on_start() override;
    void on_stop() override;
    std::string name() const override { return service_name_; }

    /// @brief Initialize the distributed actor system
    /// @return True if initialization successful
    bool initialize();

    /// @brief Shutdown the distributed actor system
    void shutdown();

    /// @brief Register a local actor with distributed discovery
    /// @param actor The CAF actor to register
    /// @param type Actor type for categorization
    /// @param name Unique actor name
    /// @param service_group Optional service group for clustering
    /// @param tags Optional metadata tags
    /// @return True if registration successful
    bool register_actor(const caf::actor &actor, ActorType type,
                        const std::string &name,
                        const std::string &service_group = "",
                        const std::map<std::string, std::string> &tags = {});

    /// @brief Unregister a local actor
    /// @param actor_name Name of the actor to unregister
    /// @return True if unregistration successful
    bool unregister_actor(const std::string &actor_name);

    /// @brief Find any actor (local or remote) by name
    /// @param actor_name Name of the actor to find
    /// @return Actor handle if found, invalid actor otherwise
    caf::actor find_actor(const std::string &actor_name);

    /// @brief Find all actors of a specific type
    /// @param type Actor type to search for
    /// @param include_local Include local actors in results
    /// @param include_remote Include remote actors in results
    /// @return Vector of matching actors
    std::vector<RegisteredActor> find_actors_by_type(
        ActorType type, bool include_local = true, bool include_remote = true);

    /// @brief Find actors by service group
    /// @param service_group Service group to search for
    /// @return Vector of actors in the service group
    std::vector<RegisteredActor> find_actors_by_group(
        const std::string &service_group);

    /// @brief Get cluster topology information
    /// @return Map of node_id -> list of actor types on that node
    std::map<std::string, std::vector<std::string>> get_cluster_topology();

    /// @brief Get cluster statistics
    /// @return Statistics about the cluster
    struct ClusterStats {
        size_t total_nodes;
        size_t total_actors;
        size_t local_actors;
        size_t remote_actors;
        std::map<std::string, size_t> actors_by_type;
        std::map<std::string, size_t> actors_by_node;
    };
    ClusterStats get_cluster_stats();

    /// @brief Send message to actor by name
    /// @param actor_name Target actor name
    /// @param message Message to send
    /// @return True if message was sent successfully
    template <typename T>
    bool send_to_actor(const std::string &actor_name, T &&message);

    /// @brief Broadcast message to all actors of a specific type
    /// @param type Target actor type
    /// @param message Message to broadcast
    /// @param include_local Include local actors
    /// @param include_remote Include remote actors
    /// @return Number of actors that received the message
    template <typename T>
    size_t broadcast_to_type(ActorType type, T &&message,
                             bool include_local = true,
                             bool include_remote = true);

    /// @brief Broadcast message to all actors in a service group
    /// @param service_group Target service group
    /// @param message Message to broadcast
    /// @return Number of actors that received the message
    template <typename T>
    size_t broadcast_to_group(const std::string &service_group, T &&message);

    /// @brief Set event callback for system events
    /// @param callback Function to call for system events
    void set_event_callback(const EventCallback &callback);

    /// @brief Check if the distributed system is healthy
    /// @return True if system is operational
    bool is_healthy() const;

    /// @brief Get current node ID
    /// @return Node identifier
    const std::string &get_node_id() const { return config_.node_id; }

    /// @brief Get the underlying CAF actor system
    /// @return Reference to the CAF actor system
    caf::actor_system &system() {
        if (!actor_system_) {
            throw std::runtime_error("Actor system not initialized");
        }
        return *actor_system_;
    }

    /// @brief Get actor registry
    /// @return Reference to the actor registry
    ActorRegistry &get_registry() { return *actor_registry_; }

    /// @brief Get actor registry (const)
    /// @return Const reference to the actor registry
    const ActorRegistry &get_registry() const { return *actor_registry_; }

private:
    /// @brief Start discovery worker thread
    void start_discovery_worker();

    /// @brief Stop discovery worker thread
    void stop_discovery_worker();

    /// @brief Discovery worker function
    void discovery_worker();

    /// @brief Handle actor discovery events
    void on_actor_discovered(const RegisteredActor &actor);

    /// @brief Handle actor removal events
    void on_actor_removed(const std::string &actor_name);

    /// @brief Discover cluster nodes
    std::vector<std::string> discover_cluster_nodes();

    /// @brief Emit system event
    void emit_event(const ActorSystemEventData &event_data);

private:
    caf::actor_system *actor_system_;
    std::shared_ptr<discovery::IServiceDiscovery> discovery_service_;
    DistributedActorConfig config_;
    std::unique_ptr<ActorRegistry> actor_registry_;
    std::string service_name_;

    // Discovery worker
    std::atomic<bool> discovery_running_{false};
    std::thread discovery_thread_;

    // Event handling
    EventCallback event_callback_;

    // State tracking
    std::atomic<bool> initialized_{false};
    std::set<std::string> known_nodes_;
    std::mutex nodes_mutex_;
};

/// @brief Template implementations

template <typename T>
bool DistributedActorSystem::send_to_actor(const std::string &actor_name,
                                           T &&message) {
    auto actor_handle = find_actor(actor_name);
    if (actor_handle) {
        caf::anon_send(actor_handle, std::forward<T>(message));
        return true;
    }
    return false;
}

template <typename T>
size_t DistributedActorSystem::broadcast_to_type(ActorType type, T &&message,
                                                 bool include_local,
                                                 bool include_remote) {
    auto actors = find_actors_by_type(type, include_local, include_remote);
    size_t sent_count = 0;

    for (const auto &registered_actor : actors) {
        if (registered_actor.actor_handle) {
            caf::anon_send(registered_actor.actor_handle, message);
            ++sent_count;
        }
    }

    return sent_count;
}

template <typename T>
size_t DistributedActorSystem::broadcast_to_group(
    const std::string &service_group, T &&message) {
    auto actors = find_actors_by_group(service_group);
    size_t sent_count = 0;

    for (const auto &registered_actor : actors) {
        if (registered_actor.actor_handle) {
            caf::anon_send(registered_actor.actor_handle, message);
            ++sent_count;
        }
    }

    return sent_count;
}

/// @brief Factory function to create distributed actor system
std::unique_ptr<DistributedActorSystem> make_distributed_actor_system(
    caf::actor_system &actor_system,
    std::shared_ptr<discovery::IServiceDiscovery> discovery_service,
    const DistributedActorConfig &config);

}  // namespace shield::actor