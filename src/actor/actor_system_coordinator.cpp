#include "shield/actor/actor_system_coordinator.hpp"

#include <iomanip>
#include <random>
#include <sstream>

#include "caf/actor_system_config.hpp"
#include "caf/io/middleman.hpp"
#include "shield/caf_type_ids.hpp"
#include "shield/discovery/etcd_discovery.hpp"
#include "shield/discovery/local_discovery.hpp"
#include "shield/log/logger.hpp"

namespace shield::actor {

ActorSystemCoordinator::ActorSystemCoordinator(const CoordinatorConfig& config)
    : config_(config) {
    // Generate random node ID if not provided
    if (config_.node_id.empty()) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(1000, 9999);

        std::ostringstream oss;
        oss << config_.node_type << "_" << dis(gen);
        config_.node_id = oss.str();
    }

    SHIELD_LOG_INFO << "ActorSystemCoordinator created for node: "
                    << config_.node_id;
}

ActorSystemCoordinator::~ActorSystemCoordinator() {
    stop();
    SHIELD_LOG_INFO << "ActorSystemCoordinator destroyed";
}

bool ActorSystemCoordinator::initialize() {
    if (initialized_) {
        SHIELD_LOG_WARNING << "ActorSystemCoordinator already initialized";
        return true;
    }

    try {
        emit_status("Initializing actor system coordinator...");

        // Create discovery service
        discovery_service_ = create_discovery_service();
        if (!discovery_service_) {
            emit_status("Failed to create discovery service", true);
            return false;
        }
        emit_status("Discovery service created: " + config_.discovery_type);

        // Create CAF actor system
        actor_system_ = create_actor_system();
        if (!actor_system_) {
            emit_status("Failed to create CAF actor system", true);
            return false;
        }
        emit_status("CAF actor system created with " +
                    std::to_string(config_.worker_threads) + " workers");

        // Create distributed actor system
        DistributedActorConfig dist_config;
        dist_config.node_id = config_.node_id;
        dist_config.cluster_name = config_.cluster_name;
        dist_config.actor_port = config_.actor_port;
        dist_config.heartbeat_interval = config_.heartbeat_interval;
        dist_config.discovery_interval = config_.discovery_interval;
        dist_config.auto_discovery = true;

        distributed_system_ = make_distributed_actor_system(
            *actor_system_, discovery_service_, dist_config);
        if (!distributed_system_) {
            emit_status("Failed to create distributed actor system", true);
            return false;
        }

        // Set up event handling
        distributed_system_->set_event_callback(
            [this](const ActorSystemEventData& event_data) {
                handle_system_event(event_data);
            });

        // Initialize distributed system
        if (!distributed_system_->initialize()) {
            emit_status("Failed to initialize distributed actor system", true);
            return false;
        }
        emit_status("Distributed actor system initialized");

        initialized_ = true;
        emit_status("ActorSystemCoordinator initialized successfully");
        return true;

    } catch (const std::exception& e) {
        std::string error_msg =
            "Exception during initialization: " + std::string(e.what());
        emit_status(error_msg, true);
        SHIELD_LOG_ERROR << error_msg;
        return false;
    }
}

bool ActorSystemCoordinator::start() {
    if (!initialized_) {
        emit_status("Cannot start: coordinator not initialized", true);
        return false;
    }

    if (running_) {
        SHIELD_LOG_WARNING << "ActorSystemCoordinator already running";
        return true;
    }

    try {
        emit_status("Starting actor system coordinator...");

        running_ = true;
        emit_status("ActorSystemCoordinator started successfully on node: " +
                    config_.node_id);

        // If auto_start is enabled, automatically start some basic services
        if (config_.auto_start) {
            emit_status("Auto-start mode enabled");
        }

        return true;

    } catch (const std::exception& e) {
        std::string error_msg =
            "Exception during start: " + std::string(e.what());
        emit_status(error_msg, true);
        SHIELD_LOG_ERROR << error_msg;
        running_ = false;
        return false;
    }
}

void ActorSystemCoordinator::stop() {
    if (!running_) {
        return;
    }

    try {
        emit_status("Stopping actor system coordinator...");

        running_ = false;

        // Shutdown distributed system
        if (distributed_system_) {
            distributed_system_->shutdown();
            emit_status("Distributed actor system shut down");
        }

        // Reset all components
        distributed_system_.reset();
        actor_system_.reset();
        discovery_service_.reset();

        initialized_ = false;
        emit_status("ActorSystemCoordinator stopped successfully");

    } catch (const std::exception& e) {
        std::string error_msg =
            "Exception during stop: " + std::string(e.what());
        emit_status(error_msg, true);
        SHIELD_LOG_ERROR << error_msg;
    }
}

bool ActorSystemCoordinator::register_actor(
    const caf::actor& actor, ActorType type, const std::string& name,
    const std::string& service_group,
    const std::map<std::string, std::string>& tags) {
    if (!running_) {
        return false;
    }

    bool success = distributed_system_->register_actor(actor, type, name,
                                                       service_group, tags);
    if (success) {
        ++total_actors_registered_;
        emit_status(
            "Actor registered: " + name + " (type: " +
            ActorMetadata{type, "", "", "", {}, 100, {}}.type_to_string() +
            ")");
    }

    return success;
}

caf::actor ActorSystemCoordinator::find_actor(const std::string& actor_name) {
    if (!running_) {
        return caf::actor{};
    }

    return distributed_system_->find_actor(actor_name);
}

std::vector<RegisteredActor> ActorSystemCoordinator::find_actors_by_type(
    ActorType type) {
    if (!running_) {
        return {};
    }

    return distributed_system_->find_actors_by_type(type);
}

std::map<std::string, std::string>
ActorSystemCoordinator::get_cluster_status() {
    std::map<std::string, std::string> status;

    status["node_id"] = config_.node_id;
    status["node_type"] = config_.node_type;
    status["cluster_name"] = config_.cluster_name;
    status["initialized"] = initialized_ ? "true" : "false";
    status["running"] = running_ ? "true" : "false";
    status["discovery_type"] = config_.discovery_type;
    status["total_actors_registered"] =
        std::to_string(total_actors_registered_.load());
    status["total_messages_sent"] = std::to_string(total_messages_sent_.load());

    if (running_ && distributed_system_) {
        auto cluster_stats = distributed_system_->get_cluster_stats();
        status["cluster_total_nodes"] =
            std::to_string(cluster_stats.total_nodes);
        status["cluster_total_actors"] =
            std::to_string(cluster_stats.total_actors);
        status["cluster_local_actors"] =
            std::to_string(cluster_stats.local_actors);
        status["cluster_remote_actors"] =
            std::to_string(cluster_stats.remote_actors);
        status["healthy"] =
            distributed_system_->is_healthy() ? "true" : "false";
    } else {
        status["healthy"] = "false";
    }

    return status;
}

void ActorSystemCoordinator::set_status_callback(
    const StatusCallback& callback) {
    status_callback_ = callback;
}

std::shared_ptr<discovery::IServiceDiscovery>
ActorSystemCoordinator::create_discovery_service() {
    if (config_.discovery_type == "etcd") {
        if (config_.discovery_endpoints.empty()) {
            SHIELD_LOG_ERROR << "ETCD endpoints not configured";
            return nullptr;
        }
        return shield::discovery::make_etcd_discovery(
            config_.discovery_endpoints);
    } else if (config_.discovery_type == "in-memory") {
        return shield::discovery::make_local_discovery();
    } else {
        SHIELD_LOG_ERROR << "Unknown discovery service type: "
                         << config_.discovery_type;
        return nullptr;
    }
}

std::unique_ptr<caf::actor_system>
ActorSystemCoordinator::create_actor_system() {
    caf::actor_system_config caf_config;

    // Configure scheduler
    caf_config.set("scheduler.max-threads", config_.worker_threads);

    // Configure logging
    caf_config.set("logger.verbosity", caf::log::level::info);

    // Configure middleman for network communication
    caf_config.load<caf::io::middleman>();

    return std::make_unique<caf::actor_system>(caf_config);
}

void ActorSystemCoordinator::handle_system_event(
    const ActorSystemEventData& event_data) {
    std::ostringstream oss;

    switch (event_data.event_type) {
        case ActorSystemEvent::NODE_JOINED:
            oss << "Node joined cluster: " << event_data.node_id;
            break;
        case ActorSystemEvent::NODE_LEFT:
            oss << "Node left cluster: " << event_data.node_id;
            break;
        case ActorSystemEvent::ACTOR_DISCOVERED:
            oss << "Actor discovered: " << event_data.actor_name << " on node "
                << event_data.node_id;
            break;
        case ActorSystemEvent::ACTOR_LOST:
            oss << "Actor lost: " << event_data.actor_name << " on node "
                << event_data.node_id;
            break;
        case ActorSystemEvent::CLUSTER_CHANGED:
            oss << "Cluster topology changed";
            break;
    }

    emit_status(oss.str());
}

void ActorSystemCoordinator::emit_status(const std::string& status,
                                         bool is_error) {
    if (is_error) {
        SHIELD_LOG_ERROR << "[Coordinator] " << status;
    } else {
        SHIELD_LOG_INFO << "[Coordinator] " << status;
    }

    if (status_callback_) {
        try {
            status_callback_(status, is_error);
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Exception in status callback: " << e.what();
        }
    }
}

std::unique_ptr<ActorSystemCoordinator> make_coordinator_from_config(
    const shield::config::Config& shield_config) {
    CoordinatorConfig config;

    // Extract configuration from Shield config
    // Note: This assumes the Shield config has been extended with actor system
    // settings
    config.worker_threads = shield_config.get<int>("caf.worker_threads");
    config.discovery_type = "in-memory";  // Default for now

    // Generate node ID based on hostname or other unique identifier
    std::ostringstream oss;
    oss << "shield_node_" << std::time(nullptr);
    config.node_id = oss.str();

    return std::make_unique<ActorSystemCoordinator>(config);
}

std::unique_ptr<ActorSystemCoordinator> make_default_coordinator(
    const std::string& node_id) {
    CoordinatorConfig config;

    if (!node_id.empty()) {
        config.node_id = node_id;
    } else {
        std::ostringstream oss;
        oss << "default_node_" << std::time(nullptr);
        config.node_id = oss.str();
    }

    config.discovery_type = "in-memory";
    config.worker_threads = 4;
    config.auto_start = true;

    return std::make_unique<ActorSystemCoordinator>(config);
}

}  // namespace shield::actor