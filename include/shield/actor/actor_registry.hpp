#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "caf/actor.hpp"
#include "caf/actor_addr.hpp"
#include "caf/actor_system.hpp"
#include "caf/typed_actor.hpp"
#include "shield/discovery/service_discovery.hpp"
#include "shield/discovery/service_instance.hpp"

namespace shield::actor {

/// @brief Actor type enumeration for different actor categories
enum class ActorType {
    GATEWAY,   // Gateway/Frontend actors handling client connections
    LOGIC,     // Game logic actors (players, rooms, etc.)
    DATABASE,  // Database service actors
    AUTH,      // Authentication service actors
    MONITOR,   // Monitoring and metrics actors
    CUSTOM     // User-defined actor types
};

/// @brief Actor metadata for enhanced discovery and routing
struct ActorMetadata {
    ActorType type;
    std::string name;                         // Actor name/identifier
    std::string node_id;                      // Node where actor is running
    std::string service_group;                // Service group for scaling
    std::map<std::string, std::string> tags;  // Custom tags for filtering
    uint32_t load_weight = 100;               // Load balancing weight
    std::chrono::steady_clock::time_point last_heartbeat;

    /// @brief Convert actor type to string
    std::string type_to_string() const;

    /// @brief Convert string to actor type
    static ActorType string_to_type(const std::string &type_str);
};

/// @brief Registered actor information
struct RegisteredActor {
    caf::actor actor_handle;  // CAF actor handle
    ActorMetadata metadata;   // Actor metadata
    std::string actor_uri;    // CAF actor URI for remote access
    bool is_local;            // Whether actor is local to this node

    RegisteredActor() = default;
    RegisteredActor(const caf::actor &actor, const ActorMetadata &meta,
                    const std::string &uri, bool local = true);
};

/// @brief Actor registry for managing local and distributed actors
class ActorRegistry {
public:
    /// @brief Callback type for actor discovery events
    using ActorDiscoveryCallback = std::function<void(const RegisteredActor &)>;

    /// @brief Callback type for actor removal events
    using ActorRemovalCallback = std::function<void(const std::string &)>;

    explicit ActorRegistry(
        caf::actor_system &system,
        std::shared_ptr<discovery::IServiceDiscovery> discovery_service,
        const std::string &node_id);

    ~ActorRegistry();

    /// @brief Register a local actor with the registry
    /// @param actor The CAF actor to register
    /// @param metadata Actor metadata including type and properties
    /// @param ttl Time-to-live for the registration (optional)
    /// @return True if registration successful
    bool register_actor(const caf::actor &actor, const ActorMetadata &metadata,
                        std::optional<std::chrono::seconds> ttl = std::nullopt);

    /// @brief Unregister an actor from the registry
    /// @param actor_name The name of the actor to unregister
    /// @return True if unregistration successful
    bool unregister_actor(const std::string &actor_name);

    /// @brief Find a local actor by name
    /// @param actor_name The name of the actor to find
    /// @return Actor handle if found, invalid actor otherwise
    caf::actor find_local_actor(const std::string &actor_name) const;

    /// @brief Find all local actors of a specific type
    /// @param type The actor type to search for
    /// @return Vector of matching actors
    std::vector<RegisteredActor> find_local_actors_by_type(
        ActorType type) const;

    /// @brief Discover remote actor by name across the cluster
    /// @param actor_name The name of the actor to discover
    /// @return Actor handle if found, invalid actor otherwise
    caf::actor discover_remote_actor(const std::string &actor_name);

    /// @brief Discover all remote actors of a specific type
    /// @param type The actor type to search for
    /// @return Vector of discovered remote actors
    std::vector<RegisteredActor> discover_remote_actors_by_type(ActorType type);

    /// @brief Discover actors by service group
    /// @param service_group The service group to search for
    /// @return Vector of actors in the service group
    std::vector<RegisteredActor> discover_actors_by_group(
        const std::string &service_group);

    /// @brief Get all locally registered actors
    /// @return Vector of all local actors
    std::vector<RegisteredActor> get_all_local_actors() const;

    /// @brief Set callback for actor discovery events
    /// @param callback Function to call when new actors are discovered
    void set_discovery_callback(const ActorDiscoveryCallback &callback);

    /// @brief Set callback for actor removal events
    /// @param callback Function to call when actors are removed
    void set_removal_callback(const ActorRemovalCallback &callback);

    /// @brief Start periodic heartbeat for registered actors
    /// @param interval Heartbeat interval
    void start_heartbeat(
        std::chrono::seconds interval = std::chrono::seconds(30));

    /// @brief Stop heartbeat service
    void stop_heartbeat();

    /// @brief Get node ID
    /// @return Current node identifier
    const std::string &get_node_id() const { return node_id_; }

    /// @brief Check if actor registry is healthy
    /// @return True if registry is operational
    bool is_healthy() const;

private:
    /// @brief Convert actor metadata to service instance
    discovery::ServiceInstance metadata_to_service_instance(
        const ActorMetadata &metadata, const std::string &actor_uri) const;

    /// @brief Convert service instance to registered actor
    RegisteredActor service_instance_to_registered_actor(
        const discovery::ServiceInstance &instance) const;

    /// @brief Generate actor URI from actor handle
    std::string generate_actor_uri(const caf::actor &actor) const;

    /// @brief Periodic heartbeat function
    void heartbeat_worker();

    /// @brief Update actor heartbeat
    void update_heartbeat(const std::string &actor_name);

private:
    caf::actor_system &actor_system_;
    std::shared_ptr<discovery::IServiceDiscovery> discovery_service_;
    std::string node_id_;

    mutable std::mutex registry_mutex_;
    std::unordered_map<std::string, RegisteredActor>
        local_actors_;  // actor_name -> RegisteredActor
    std::unordered_map<std::string, RegisteredActor>
        remote_actors_cache_;  // actor_name -> RegisteredActor

    // Callbacks
    ActorDiscoveryCallback discovery_callback_;
    ActorRemovalCallback removal_callback_;

    // Heartbeat management
    std::atomic<bool> heartbeat_running_{false};
    std::thread heartbeat_thread_;
    std::chrono::seconds heartbeat_interval_;
};

/// @brief Factory function to create actor registry
std::unique_ptr<ActorRegistry> make_actor_registry(
    caf::actor_system &system,
    std::shared_ptr<discovery::IServiceDiscovery> discovery_service,
    const std::string &node_id);

}  // namespace shield::actor