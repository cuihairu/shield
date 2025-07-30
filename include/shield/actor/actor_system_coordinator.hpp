#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "caf/actor_system.hpp"
#include "shield/actor/distributed_actor_system.hpp"
#include "shield/config/config.hpp"
#include "shield/discovery/service_discovery.hpp"

namespace shield::actor {

/// @brief Actor system coordinator configuration
struct CoordinatorConfig {
    std::string node_id;
    std::string node_type =
        "logic";  // Node type: gateway, logic, database, etc.
    std::string cluster_name = "shield_cluster";
    std::string discovery_type = "in-memory";  // Discovery service type
    std::string discovery_endpoints = "";      // Discovery service endpoints
    uint16_t actor_port = 0;                   // Actor communication port
    size_t worker_threads = 4;                 // CAF worker threads
    bool auto_start = true;                    // Auto-start the coordinator
    std::chrono::seconds heartbeat_interval{30};
    std::chrono::seconds discovery_interval{60};
};

/// @brief Central coordinator for managing the complete actor system
/// Provides a unified interface for initializing and managing all actor-related
/// components
class ActorSystemCoordinator {
public:
    /// @brief Status callback type
    using StatusCallback =
        std::function<void(const std::string &status, bool is_error)>;

    /// @brief Constructor
    /// @param config Coordinator configuration
    explicit ActorSystemCoordinator(const CoordinatorConfig &config);

    /// @brief Destructor
    ~ActorSystemCoordinator();

    /// @brief Initialize the complete actor system
    /// @return True if initialization successful
    bool initialize();

    /// @brief Start the actor system
    /// @return True if start successful
    bool start();

    /// @brief Stop the actor system
    void stop();

    /// @brief Check if the system is running
    /// @return True if system is running
    bool is_running() const { return running_; }

    /// @brief Get the CAF actor system
    /// @return Reference to the actor system
    caf::actor_system &get_actor_system() { return *actor_system_; }

    /// @brief Get the distributed actor system
    /// @return Reference to the distributed actor system
    DistributedActorSystem &get_distributed_system() {
        return *distributed_system_;
    }

    /// @brief Get the discovery service
    /// @return Shared pointer to the discovery service
    std::shared_ptr<discovery::IServiceDiscovery> get_discovery_service() {
        return discovery_service_;
    }

    /// @brief Register an actor with the system
    /// @param actor The CAF actor to register
    /// @param type Actor type
    /// @param name Unique actor name
    /// @param service_group Optional service group
    /// @param tags Optional metadata tags
    /// @return True if registration successful
    bool register_actor(const caf::actor &actor, ActorType type,
                        const std::string &name,
                        const std::string &service_group = "",
                        const std::map<std::string, std::string> &tags = {});

    /// @brief Create and register a typed actor
    /// @tparam ActorImpl Actor implementation class
    /// @tparam Args Constructor arguments
    /// @param type Actor type
    /// @param name Unique actor name
    /// @param service_group Optional service group
    /// @param tags Optional metadata tags
    /// @param args Constructor arguments
    /// @return The created actor handle
    template <typename ActorImpl, typename... Args>
    caf::actor spawn_and_register(
        ActorType type, const std::string &name,
        const std::string &service_group = "",
        const std::map<std::string, std::string> &tags = {}, Args &&...args);

    /// @brief Find an actor by name
    /// @param actor_name Name of the actor to find
    /// @return Actor handle if found
    caf::actor find_actor(const std::string &actor_name);

    /// @brief Find actors by type
    /// @param type Actor type to search for
    /// @return Vector of matching actors
    std::vector<RegisteredActor> find_actors_by_type(ActorType type);

    /// @brief Send message to actor by name
    /// @param actor_name Target actor name
    /// @param message Message to send
    /// @return True if message was sent
    template <typename T>
    bool send_to_actor(const std::string &actor_name, T &&message);

    /// @brief Broadcast message to actors of a type
    /// @param type Target actor type
    /// @param message Message to broadcast
    /// @return Number of actors that received the message
    template <typename T>
    size_t broadcast_to_type(ActorType type, T &&message);

    /// @brief Get cluster status information
    /// @return Cluster status as JSON-like map
    std::map<std::string, std::string> get_cluster_status();

    /// @brief Set status callback for system events
    /// @param callback Function to call for status updates
    void set_status_callback(const StatusCallback &callback);

    /// @brief Get current node ID
    /// @return Node identifier
    const std::string &get_node_id() const { return config_.node_id; }

    /// @brief Get coordinator configuration
    /// @return Current configuration
    const CoordinatorConfig &get_config() const { return config_; }

private:
    /// @brief Create discovery service based on configuration
    /// @return Discovery service instance
    std::shared_ptr<discovery::IServiceDiscovery> create_discovery_service();

    /// @brief Create CAF actor system with configuration
    /// @return CAF actor system instance
    std::unique_ptr<caf::actor_system> create_actor_system();

    /// @brief Handle system events
    /// @param event_data Event information
    void handle_system_event(const ActorSystemEventData &event_data);

    /// @brief Emit status update
    /// @param status Status message
    /// @param is_error Whether this is an error status
    void emit_status(const std::string &status, bool is_error = false);

private:
    CoordinatorConfig config_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};

    // Core components
    std::unique_ptr<caf::actor_system> actor_system_;
    std::shared_ptr<discovery::IServiceDiscovery> discovery_service_;
    std::unique_ptr<DistributedActorSystem> distributed_system_;

    // Callbacks
    StatusCallback status_callback_;

    // Statistics
    std::atomic<size_t> total_actors_registered_{0};
    std::atomic<size_t> total_messages_sent_{0};
};

/// @brief Template implementations

template <typename ActorImpl, typename... Args>
caf::actor ActorSystemCoordinator::spawn_and_register(
    ActorType type, const std::string &name, const std::string &service_group,
    const std::map<std::string, std::string> &tags, Args &&...args) {
    if (!running_) {
        return caf::actor{};
    }

    auto actor = actor_system_->spawn<ActorImpl>(std::forward<Args>(args)...);
    if (actor) {
        if (register_actor(actor, type, name, service_group, tags)) {
            return actor;
        }
    }

    return caf::actor{};
}

template <typename T>
bool ActorSystemCoordinator::send_to_actor(const std::string &actor_name,
                                           T &&message) {
    if (!running_) {
        return false;
    }

    bool result = distributed_system_->send_to_actor(actor_name,
                                                     std::forward<T>(message));
    if (result) {
        ++total_messages_sent_;
    }
    return result;
}

template <typename T>
size_t ActorSystemCoordinator::broadcast_to_type(ActorType type, T &&message) {
    if (!running_) {
        return 0;
    }

    size_t count =
        distributed_system_->broadcast_to_type(type, std::forward<T>(message));
    total_messages_sent_ += count;
    return count;
}

/// @brief Factory function to create coordinator from Shield config
std::unique_ptr<ActorSystemCoordinator> make_coordinator_from_config(
    const shield::config::Config &shield_config);

/// @brief Factory function to create coordinator with default configuration
std::unique_ptr<ActorSystemCoordinator> make_default_coordinator(
    const std::string &node_id = "");

}  // namespace shield::actor