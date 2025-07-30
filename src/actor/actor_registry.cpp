#include "shield/actor/actor_registry.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <thread>

#include "caf/io/all.hpp"
#include "caf/io/middleman.hpp"
#include "caf/uri.hpp"
#include "shield/caf_type_ids.hpp"
#include "shield/log/logger.hpp"

namespace shield::actor {

std::string ActorMetadata::type_to_string() const {
    switch (type) {
        case ActorType::GATEWAY:
            return "gateway";
        case ActorType::LOGIC:
            return "logic";
        case ActorType::DATABASE:
            return "database";
        case ActorType::AUTH:
            return "auth";
        case ActorType::MONITOR:
            return "monitor";
        case ActorType::CUSTOM:
            return "custom";
        default:
            return "unknown";
    }
}

ActorType ActorMetadata::string_to_type(const std::string& type_str) {
    if (type_str == "gateway") return ActorType::GATEWAY;
    if (type_str == "logic") return ActorType::LOGIC;
    if (type_str == "database") return ActorType::DATABASE;
    if (type_str == "auth") return ActorType::AUTH;
    if (type_str == "monitor") return ActorType::MONITOR;
    if (type_str == "custom") return ActorType::CUSTOM;
    return ActorType::CUSTOM;  // Default to custom for unknown types
}

RegisteredActor::RegisteredActor(const caf::actor& actor,
                                 const ActorMetadata& meta,
                                 const std::string& uri, bool local)
    : actor_handle(actor), metadata(meta), actor_uri(uri), is_local(local) {}

ActorRegistry::ActorRegistry(
    caf::actor_system& system,
    std::shared_ptr<discovery::IServiceDiscovery> discovery_service,
    const std::string& node_id)
    : actor_system_(system),
      discovery_service_(discovery_service),
      node_id_(node_id),
      heartbeat_interval_(std::chrono::seconds(30)) {
    SHIELD_LOG_INFO << "ActorRegistry initialized for node: " << node_id_;
}

ActorRegistry::~ActorRegistry() {
    stop_heartbeat();

    // Unregister all local actors
    std::lock_guard<std::mutex> lock(registry_mutex_);
    for (const auto& [name, registered_actor] : local_actors_) {
        discovery_service_->deregister_service(
            "actor:" + registered_actor.metadata.type_to_string(),
            registered_actor.metadata.name);
    }

    SHIELD_LOG_INFO << "ActorRegistry destroyed";
}

bool ActorRegistry::register_actor(const caf::actor& actor,
                                   const ActorMetadata& metadata,
                                   std::optional<std::chrono::seconds> ttl) {
    if (!actor) {
        SHIELD_LOG_ERROR << "Cannot register invalid actor: " << metadata.name;
        return false;
    }

    std::string actor_uri = generate_actor_uri(actor);
    if (actor_uri.empty()) {
        SHIELD_LOG_ERROR << "Failed to generate URI for actor: "
                         << metadata.name;
        return false;
    }

    // Create service instance for discovery
    auto service_instance = metadata_to_service_instance(metadata, actor_uri);

    // Register with discovery service
    std::string service_name = "actor:" + metadata.type_to_string();
    if (!discovery_service_->register_service(service_instance, ttl)) {
        SHIELD_LOG_ERROR << "Failed to register actor with discovery service: "
                         << metadata.name;
        return false;
    }

    // Add to local registry
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        RegisteredActor registered_actor(actor, metadata, actor_uri, true);
        registered_actor.metadata.last_heartbeat =
            std::chrono::steady_clock::now();
        local_actors_[metadata.name] = std::move(registered_actor);
    }

    SHIELD_LOG_INFO << "Actor registered successfully: " << metadata.name
                    << " (type: " << metadata.type_to_string()
                    << ", uri: " << actor_uri << ")";
    return true;
}

bool ActorRegistry::unregister_actor(const std::string& actor_name) {
    std::lock_guard<std::mutex> lock(registry_mutex_);

    auto it = local_actors_.find(actor_name);
    if (it == local_actors_.end()) {
        SHIELD_LOG_WARNING << "Actor not found for unregistration: "
                           << actor_name;
        return false;
    }

    const auto& registered_actor = it->second;
    std::string service_name =
        "actor:" + registered_actor.metadata.type_to_string();

    // Deregister from discovery service
    if (!discovery_service_->deregister_service(service_name, actor_name)) {
        SHIELD_LOG_WARNING
            << "Failed to deregister actor from discovery service: "
            << actor_name;
    }

    // Remove from local registry
    local_actors_.erase(it);

    // Trigger removal callback
    if (removal_callback_) {
        removal_callback_(actor_name);
    }

    SHIELD_LOG_INFO << "Actor unregistered: " << actor_name;
    return true;
}

caf::actor ActorRegistry::find_local_actor(
    const std::string& actor_name) const {
    std::lock_guard<std::mutex> lock(registry_mutex_);

    auto it = local_actors_.find(actor_name);
    if (it != local_actors_.end()) {
        return it->second.actor_handle;
    }

    return caf::actor{};  // Return invalid actor
}

std::vector<RegisteredActor> ActorRegistry::find_local_actors_by_type(
    ActorType type) const {
    std::vector<RegisteredActor> result;
    std::lock_guard<std::mutex> lock(registry_mutex_);

    for (const auto& [name, registered_actor] : local_actors_) {
        if (registered_actor.metadata.type == type) {
            result.push_back(registered_actor);
        }
    }

    return result;
}

caf::actor ActorRegistry::discover_remote_actor(const std::string& actor_name) {
    // First check local cache
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = remote_actors_cache_.find(actor_name);
        if (it != remote_actors_cache_.end()) {
            return it->second.actor_handle;
        }
    }

    // Search across all actor service types
    std::vector<std::string> service_types = {"gateway", "logic",   "database",
                                              "auth",    "monitor", "custom"};

    for (const auto& service_type : service_types) {
        std::string service_name = "actor:" + service_type;
        auto instances = discovery_service_->query_all_services(service_name);

        for (const auto& instance : instances) {
            if (instance.instance_id == actor_name) {
                // Found the actor, try to connect to it
                auto uri_result = caf::make_uri(instance.address);
                if (uri_result) {
                    auto& uri = *uri_result;
                    auto remote_actor = actor_system_.middleman().remote_actor(
                        uri.authority().host_str(), uri.authority().port);
                    if (remote_actor) {
                        // Cache the remote actor
                        RegisteredActor registered_actor =
                            service_instance_to_registered_actor(instance);
                        registered_actor.actor_handle =
                            caf::actor_cast<caf::actor>(*remote_actor);

                        std::lock_guard<std::mutex> lock(registry_mutex_);
                        remote_actors_cache_[actor_name] = registered_actor;

                        if (discovery_callback_) {
                            discovery_callback_(registered_actor);
                        }

                        SHIELD_LOG_INFO
                            << "Discovered remote actor: " << actor_name
                            << " at " << instance.address;
                        return *remote_actor;
                    }
                }
            }
        }
    }

    SHIELD_LOG_WARNING << "Remote actor not found: " << actor_name;
    return caf::actor{};
}

std::vector<RegisteredActor> ActorRegistry::discover_remote_actors_by_type(
    ActorType type) {
    std::vector<RegisteredActor> result;
    std::string service_name =
        "actor:" +
        ActorMetadata{type, "", "", "", {}, 100, {}}.type_to_string();

    auto instances = discovery_service_->query_all_services(service_name);

    for (const auto& instance : instances) {
        // Skip local actors
        if (instance.metadata.custom_attributes.count("node_id") &&
            instance.metadata.custom_attributes.at("node_id") == node_id_) {
            continue;
        }

        auto uri_result = caf::make_uri(instance.address);
        if (uri_result) {
            auto& uri = *uri_result;
            auto remote_actor = actor_system_.middleman().remote_actor(
                uri.authority().host_str(), uri.authority().port);
            if (remote_actor) {
                RegisteredActor registered_actor =
                    service_instance_to_registered_actor(instance);
                registered_actor.actor_handle =
                    caf::actor_cast<caf::actor>(*remote_actor);
                result.push_back(registered_actor);

                // Cache the remote actor
                std::lock_guard<std::mutex> lock(registry_mutex_);
                remote_actors_cache_[registered_actor.metadata.name] =
                    registered_actor;
            }
        }
    }

    SHIELD_LOG_DEBUG
        << "Discovered " << result.size() << " remote actors of type: "
        << ActorMetadata{type, "", "", "", {}, 100, {}}.type_to_string();
    return result;
}

std::vector<RegisteredActor> ActorRegistry::discover_actors_by_group(
    const std::string& service_group) {
    std::vector<RegisteredActor> result;

    // Search across all actor service types
    std::vector<std::string> service_types = {"gateway", "logic",   "database",
                                              "auth",    "monitor", "custom"};

    for (const auto& service_type : service_types) {
        std::string service_name = "actor:" + service_type;
        auto instances = discovery_service_->query_all_services(service_name);

        for (const auto& instance : instances) {
            if (instance.metadata.custom_attributes.count("service_group") &&
                instance.metadata.custom_attributes.at("service_group") ==
                    service_group) {
                RegisteredActor registered_actor =
                    service_instance_to_registered_actor(instance);

                // Try to get actor handle if it's remote
                if (instance.metadata.custom_attributes.count("node_id") &&
                    instance.metadata.custom_attributes.at("node_id") !=
                        node_id_) {
                    auto uri_result = caf::make_uri(instance.address);
                    if (uri_result) {
                        auto& uri = *uri_result;
                        auto remote_actor =
                            actor_system_.middleman().remote_actor(
                                uri.authority().host_str(),
                                uri.authority().port);
                        if (remote_actor) {
                            registered_actor.actor_handle =
                                caf::actor_cast<caf::actor>(*remote_actor);
                        }
                    }
                } else {
                    // Local actor
                    registered_actor.actor_handle =
                        find_local_actor(registered_actor.metadata.name);
                }

                result.push_back(registered_actor);
            }
        }
    }

    return result;
}

std::vector<RegisteredActor> ActorRegistry::get_all_local_actors() const {
    std::vector<RegisteredActor> result;
    std::lock_guard<std::mutex> lock(registry_mutex_);

    result.reserve(local_actors_.size());
    for (const auto& [name, registered_actor] : local_actors_) {
        result.push_back(registered_actor);
    }

    return result;
}

void ActorRegistry::set_discovery_callback(
    const ActorDiscoveryCallback& callback) {
    discovery_callback_ = callback;
}

void ActorRegistry::set_removal_callback(const ActorRemovalCallback& callback) {
    removal_callback_ = callback;
}

void ActorRegistry::start_heartbeat(std::chrono::seconds interval) {
    heartbeat_interval_ = interval;
    heartbeat_running_ = true;
    heartbeat_thread_ = std::thread([this]() { heartbeat_worker(); });

    SHIELD_LOG_INFO << "Actor registry heartbeat started with interval: "
                    << interval.count() << "s";
}

void ActorRegistry::stop_heartbeat() {
    if (heartbeat_running_) {
        heartbeat_running_ = false;
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
        SHIELD_LOG_INFO << "Actor registry heartbeat stopped";
    }
}

bool ActorRegistry::is_healthy() const {
    return discovery_service_ != nullptr && heartbeat_running_;
}

discovery::ServiceInstance ActorRegistry::metadata_to_service_instance(
    const ActorMetadata& metadata, const std::string& actor_uri) const {
    discovery::ServiceInstance instance;
    instance.service_name = "actor:" + metadata.type_to_string();
    instance.instance_id = metadata.name;
    instance.address = actor_uri;

    // Convert actor metadata to service metadata
    instance.metadata.version = "1.0.0";     // Default version
    instance.metadata.region = "local";      // Default region
    instance.metadata.environment = "prod";  // Default environment
    instance.metadata.weight = metadata.load_weight;
    instance.metadata.tags = {"actor", metadata.type_to_string()};

    // Add custom attributes
    instance.metadata.custom_attributes["node_id"] = metadata.node_id;
    instance.metadata.custom_attributes["service_group"] =
        metadata.service_group;
    instance.metadata.custom_attributes["actor_type"] =
        metadata.type_to_string();

    // Add user-defined tags
    for (const auto& [key, value] : metadata.tags) {
        instance.metadata.custom_attributes[key] = value;
    }

    // Set expiration time if TTL heartbeat is used
    instance.expiration_time = std::chrono::steady_clock::time_point::max();

    return instance;
}

RegisteredActor ActorRegistry::service_instance_to_registered_actor(
    const discovery::ServiceInstance& instance) const {
    RegisteredActor registered_actor;
    registered_actor.actor_uri = instance.address;
    registered_actor.is_local = false;  // Assume remote unless proven otherwise

    // Convert service metadata to actor metadata
    registered_actor.metadata.name = instance.instance_id;
    registered_actor.metadata.node_id =
        instance.metadata.custom_attributes.count("node_id")
            ? instance.metadata.custom_attributes.at("node_id")
            : "";
    registered_actor.metadata.service_group =
        instance.metadata.custom_attributes.count("service_group")
            ? instance.metadata.custom_attributes.at("service_group")
            : "";
    registered_actor.metadata.load_weight = instance.metadata.weight;

    // Convert actor type
    if (instance.metadata.custom_attributes.count("actor_type")) {
        registered_actor.metadata.type = ActorMetadata::string_to_type(
            instance.metadata.custom_attributes.at("actor_type"));
    } else {
        registered_actor.metadata.type = ActorType::CUSTOM;
    }

    // Copy custom attributes as tags
    for (const auto& [key, value] : instance.metadata.custom_attributes) {
        if (key != "node_id" && key != "service_group" && key != "actor_type") {
            registered_actor.metadata.tags[key] = value;
        }
    }

    registered_actor.metadata.last_heartbeat = std::chrono::steady_clock::now();

    return registered_actor;
}

std::string ActorRegistry::generate_actor_uri(const caf::actor& actor) const {
    try {
        // Publish the actor to make it accessible remotely
        auto port = actor_system_.middleman().publish(
            actor, 0);  // 0 = any available port
        if (!port) {
            SHIELD_LOG_ERROR << "Failed to publish actor: "
                             << to_string(port.error());
            return "";
        }

        // Generate URI in the format: tcp://hostname:port/actor_id
        std::ostringstream uri_stream;
        uri_stream << "tcp://localhost:" << *port;

        return uri_stream.str();
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Exception while generating actor URI: "
                         << e.what();
        return "";
    }
}

void ActorRegistry::heartbeat_worker() {
    while (heartbeat_running_) {
        try {
            std::vector<std::string> actors_to_update;

            // Collect actor names to update
            {
                std::lock_guard<std::mutex> lock(registry_mutex_);
                actors_to_update.reserve(local_actors_.size());
                for (const auto& [name, registered_actor] : local_actors_) {
                    actors_to_update.push_back(name);
                }
            }

            // Update heartbeats for all local actors
            for (const auto& actor_name : actors_to_update) {
                update_heartbeat(actor_name);
            }

            SHIELD_LOG_DEBUG << "Heartbeat sent for " << actors_to_update.size()
                             << " actors";

        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Exception in heartbeat worker: " << e.what();
        }

        // Sleep for the heartbeat interval
        std::this_thread::sleep_for(heartbeat_interval_);
    }
}

void ActorRegistry::update_heartbeat(const std::string& actor_name) {
    std::lock_guard<std::mutex> lock(registry_mutex_);

    auto it = local_actors_.find(actor_name);
    if (it != local_actors_.end()) {
        it->second.metadata.last_heartbeat = std::chrono::steady_clock::now();

        // Re-register with discovery service to update TTL
        auto service_instance = metadata_to_service_instance(
            it->second.metadata, it->second.actor_uri);
        std::string service_name =
            "actor:" + it->second.metadata.type_to_string();

        discovery_service_->register_service(
            service_instance,
            heartbeat_interval_ * 2);  // TTL = 2x heartbeat interval
    }
}

std::unique_ptr<ActorRegistry> make_actor_registry(
    caf::actor_system& system,
    std::shared_ptr<discovery::IServiceDiscovery> discovery_service,
    const std::string& node_id) {
    return std::make_unique<ActorRegistry>(system, discovery_service, node_id);
}

}  // namespace shield::actor