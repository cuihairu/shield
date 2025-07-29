#include "shield/actor/distributed_actor_system.hpp"
#include "shield/caf_type_ids.hpp"
#include "shield/core/logger.hpp"
#include <algorithm>
#include <random>
#include <sstream>

namespace shield::actor {

DistributedActorSystem::DistributedActorSystem(
    caf::actor_system& actor_system,
    std::shared_ptr<discovery::IServiceDiscovery> discovery_service,
    const DistributedActorConfig& config)
    : actor_system_(actor_system), discovery_service_(discovery_service), config_(config) {
    
    SHIELD_LOG_INFO << "DistributedActorSystem created for node: " << config_.node_id;
}

DistributedActorSystem::~DistributedActorSystem() {
    shutdown();
    SHIELD_LOG_INFO << "DistributedActorSystem destroyed";
}

bool DistributedActorSystem::initialize() {
    if (initialized_) {
        SHIELD_LOG_WARNING << "DistributedActorSystem already initialized";
        return true;
    }

    try {
        // Create actor registry
        actor_registry_ = make_actor_registry(actor_system_, discovery_service_, config_.node_id);
        
        // Set up callbacks
        actor_registry_->set_discovery_callback(
            [this](const RegisteredActor& actor) { on_actor_discovered(actor); });
        actor_registry_->set_removal_callback(
            [this](const std::string& actor_name) { on_actor_removed(actor_name); });

        // Start heartbeat
        actor_registry_->start_heartbeat(config_.heartbeat_interval);

        // Start discovery worker if auto-discovery is enabled
        if (config_.auto_discovery) {
            start_discovery_worker();
        }

        initialized_ = true;
        
        // Emit node joined event
        ActorSystemEventData event_data;
        event_data.event_type = ActorSystemEvent::NODE_JOINED;
        event_data.node_id = config_.node_id;
        event_data.timestamp = std::chrono::steady_clock::now();
        emit_event(event_data);

        SHIELD_LOG_INFO << "DistributedActorSystem initialized successfully";
        return true;

    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to initialize DistributedActorSystem: " << e.what();
        return false;
    }
}

void DistributedActorSystem::shutdown() {
    if (!initialized_) {
        return;
    }

    try {
        // Stop discovery worker
        stop_discovery_worker();

        // Emit node left event
        ActorSystemEventData event_data;
        event_data.event_type = ActorSystemEvent::NODE_LEFT;
        event_data.node_id = config_.node_id;
        event_data.timestamp = std::chrono::steady_clock::now();
        emit_event(event_data);

        // Shutdown actor registry (this will unregister all actors)
        actor_registry_.reset();

        initialized_ = false;
        SHIELD_LOG_INFO << "DistributedActorSystem shut down successfully";

    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Error during DistributedActorSystem shutdown: " << e.what();
    }
}

bool DistributedActorSystem::register_actor(const caf::actor& actor,
                                          ActorType type,
                                          const std::string& name,
                                          const std::string& service_group,
                                          const std::map<std::string, std::string>& tags) {
    if (!initialized_) {
        SHIELD_LOG_ERROR << "DistributedActorSystem not initialized";
        return false;
    }

    ActorMetadata metadata;
    metadata.type = type;
    metadata.name = name;
    metadata.node_id = config_.node_id;
    metadata.service_group = service_group;
    metadata.tags = tags;
    metadata.last_heartbeat = std::chrono::steady_clock::now();

    bool success = actor_registry_->register_actor(actor, metadata, config_.heartbeat_interval * 2);
    
    if (success) {
        // Emit actor discovered event
        ActorSystemEventData event_data;
        event_data.event_type = ActorSystemEvent::ACTOR_DISCOVERED;
        event_data.node_id = config_.node_id;
        event_data.actor_name = name;
        event_data.actor_type = type;
        event_data.timestamp = std::chrono::steady_clock::now();
        for (const auto& [key, value] : tags) {
            event_data.metadata[key] = value;
        }
        emit_event(event_data);
    }

    return success;
}

bool DistributedActorSystem::unregister_actor(const std::string& actor_name) {
    if (!initialized_) {
        SHIELD_LOG_ERROR << "DistributedActorSystem not initialized";
        return false;
    }

    return actor_registry_->unregister_actor(actor_name);
}

caf::actor DistributedActorSystem::find_actor(const std::string& actor_name) {
    if (!initialized_) {
        SHIELD_LOG_ERROR << "DistributedActorSystem not initialized";
        return caf::actor{};
    }

    // First try local registry
    auto local_actor = actor_registry_->find_local_actor(actor_name);
    if (local_actor) {
        return local_actor;
    }

    // Then try remote discovery
    return actor_registry_->discover_remote_actor(actor_name);
}

std::vector<RegisteredActor> DistributedActorSystem::find_actors_by_type(
    ActorType type, bool include_local, bool include_remote) {
    
    std::vector<RegisteredActor> result;
    
    if (!initialized_) {
        SHIELD_LOG_ERROR << "DistributedActorSystem not initialized";
        return result;
    }

    if (include_local) {
        auto local_actors = actor_registry_->find_local_actors_by_type(type);
        result.insert(result.end(), local_actors.begin(), local_actors.end());
    }

    if (include_remote) {
        auto remote_actors = actor_registry_->discover_remote_actors_by_type(type);
        result.insert(result.end(), remote_actors.begin(), remote_actors.end());
    }

    return result;
}

std::vector<RegisteredActor> DistributedActorSystem::find_actors_by_group(
    const std::string& service_group) {
    
    if (!initialized_) {
        SHIELD_LOG_ERROR << "DistributedActorSystem not initialized";
        return {};
    }

    return actor_registry_->discover_actors_by_group(service_group);
}

std::map<std::string, std::vector<std::string>> DistributedActorSystem::get_cluster_topology() {
    std::map<std::string, std::vector<std::string>> topology;
    
    if (!initialized_) {
        return topology;
    }

    // Discover all nodes in the cluster
    auto cluster_nodes = discover_cluster_nodes();
    
    // For each node, find what actor types are running there
    std::vector<std::string> actor_type_names = {"gateway", "logic", "database", "auth", "monitor", "custom"};
    
    for (const auto& node_id : cluster_nodes) {
        std::vector<std::string> node_actor_types;
        
        for (const auto& type_name : actor_type_names) {
            std::string service_name = "actor:" + type_name;
            auto instances = discovery_service_->query_all_services(service_name);
            
            for (const auto& instance : instances) {
                if (instance.metadata.custom_attributes.count("node_id") &&
                    instance.metadata.custom_attributes.at("node_id") == node_id) {
                    if (std::find(node_actor_types.begin(), node_actor_types.end(), type_name) == 
                        node_actor_types.end()) {
                        node_actor_types.push_back(type_name);
                    }
                }
            }
        }
        
        topology[node_id] = node_actor_types;
    }
    
    return topology;
}

DistributedActorSystem::ClusterStats DistributedActorSystem::get_cluster_stats() {
    ClusterStats stats{};
    
    if (!initialized_) {
        return stats;
    }

    // Get local actors
    auto local_actors = actor_registry_->get_all_local_actors();
    stats.local_actors = local_actors.size();
    
    // Count by type for local actors
    for (const auto& actor : local_actors) {
        std::string type_name = actor.metadata.type_to_string();
        stats.actors_by_type[type_name]++;
        stats.actors_by_node[config_.node_id]++;
    }

    // Discover remote actors across all types
    std::vector<ActorType> types = {
        ActorType::GATEWAY, ActorType::LOGIC, ActorType::DATABASE,
        ActorType::AUTH, ActorType::MONITOR, ActorType::CUSTOM
    };
    
    std::set<std::string> unique_nodes;
    
    for (auto type : types) {
        auto remote_actors = actor_registry_->discover_remote_actors_by_type(type);
        stats.remote_actors += remote_actors.size();
        
        std::string type_name = ActorMetadata{type, "", "", "", {}, 100, {}}.type_to_string();
        stats.actors_by_type[type_name] += remote_actors.size();
        
        for (const auto& actor : remote_actors) {
            stats.actors_by_node[actor.metadata.node_id]++;
            unique_nodes.insert(actor.metadata.node_id);
        }
    }
    
    // Add current node to unique nodes
    unique_nodes.insert(config_.node_id);
    
    stats.total_nodes = unique_nodes.size();
    stats.total_actors = stats.local_actors + stats.remote_actors;
    
    return stats;
}

void DistributedActorSystem::set_event_callback(const EventCallback& callback) {
    event_callback_ = callback;
}

bool DistributedActorSystem::is_healthy() const {
    return initialized_ && actor_registry_ && actor_registry_->is_healthy();
}

void DistributedActorSystem::start_discovery_worker() {
    discovery_running_ = true;
    discovery_thread_ = std::thread([this]() { discovery_worker(); });
    SHIELD_LOG_INFO << "Discovery worker started";
}

void DistributedActorSystem::stop_discovery_worker() {
    if (discovery_running_) {
        discovery_running_ = false;
        if (discovery_thread_.joinable()) {
            discovery_thread_.join();
        }
        SHIELD_LOG_INFO << "Discovery worker stopped";
    }
}

void DistributedActorSystem::discovery_worker() {
    while (discovery_running_) {
        try {
            // Discover cluster nodes
            auto current_nodes = discover_cluster_nodes();
            
            {
                std::lock_guard<std::mutex> lock(nodes_mutex_);
                
                // Check for new nodes
                for (const auto& node_id : current_nodes) {
                    if (known_nodes_.find(node_id) == known_nodes_.end()) {
                        known_nodes_.insert(node_id);
                        
                        // Emit node joined event
                        ActorSystemEventData event_data;
                        event_data.event_type = ActorSystemEvent::NODE_JOINED;
                        event_data.node_id = node_id;
                        event_data.timestamp = std::chrono::steady_clock::now();
                        emit_event(event_data);
                    }
                }
                
                // Check for removed nodes
                auto it = known_nodes_.begin();
                while (it != known_nodes_.end()) {
                    if (std::find(current_nodes.begin(), current_nodes.end(), *it) == current_nodes.end()) {
                        // Node left
                        ActorSystemEventData event_data;
                        event_data.event_type = ActorSystemEvent::NODE_LEFT;
                        event_data.node_id = *it;
                        event_data.timestamp = std::chrono::steady_clock::now();
                        emit_event(event_data);
                        
                        it = known_nodes_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            
            SHIELD_LOG_DEBUG << "Discovery refresh completed. Known nodes: " << current_nodes.size();
            
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Exception in discovery worker: " << e.what();
        }
        
        std::this_thread::sleep_for(config_.discovery_interval);
    }
}

void DistributedActorSystem::on_actor_discovered(const RegisteredActor& actor) {
    ActorSystemEventData event_data;
    event_data.event_type = ActorSystemEvent::ACTOR_DISCOVERED;
    event_data.node_id = actor.metadata.node_id;
    event_data.actor_name = actor.metadata.name;
    event_data.actor_type = actor.metadata.type;
    event_data.timestamp = std::chrono::steady_clock::now();
    
    for (const auto& [key, value] : actor.metadata.tags) {
        event_data.metadata[key] = value;
    }
    
    emit_event(event_data);
}

void DistributedActorSystem::on_actor_removed(const std::string& actor_name) {
    ActorSystemEventData event_data;
    event_data.event_type = ActorSystemEvent::ACTOR_LOST;
    event_data.node_id = config_.node_id;
    event_data.actor_name = actor_name;
    event_data.timestamp = std::chrono::steady_clock::now();
    
    emit_event(event_data);
}

std::vector<std::string> DistributedActorSystem::discover_cluster_nodes() {
    std::set<std::string> unique_nodes;
    
    // Add current node
    unique_nodes.insert(config_.node_id);
    
    // Search across all actor service types to find nodes
    std::vector<std::string> service_types = {"gateway", "logic", "database", "auth", "monitor", "custom"};
    
    for (const auto& service_type : service_types) {
        std::string service_name = "actor:" + service_type;
        auto instances = discovery_service_->query_all_services(service_name);
        
        for (const auto& instance : instances) {
            if (instance.metadata.custom_attributes.count("node_id")) {
                unique_nodes.insert(instance.metadata.custom_attributes.at("node_id"));
            }
        }
    }
    
    return std::vector<std::string>(unique_nodes.begin(), unique_nodes.end());
}

void DistributedActorSystem::emit_event(const ActorSystemEventData& event_data) {
    if (event_callback_) {
        try {
            event_callback_(event_data);
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Exception in event callback: " << e.what();
        }
    }
}

std::unique_ptr<DistributedActorSystem> make_distributed_actor_system(
    caf::actor_system& actor_system,
    std::shared_ptr<discovery::IServiceDiscovery> discovery_service,
    const DistributedActorConfig& config) {
    
    return std::make_unique<DistributedActorSystem>(actor_system, discovery_service, config);
}

} // namespace shield::actor