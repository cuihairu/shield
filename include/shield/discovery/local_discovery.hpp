// shield/include/shield/discovery/in_memory_discovery.hpp
#pragma once

#include "shield/discovery/service_discovery.hpp"
#include <map>
#include <unordered_map>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <random>
#include <mutex>

namespace shield::discovery {

/// @brief A local, file-backed implementation of the service discovery interface.
/// Ideal for local development, unit/integration testing, and simple deployments.
/// This implementation is thread-safe, supports TTL-based service expiration, and persists data to a local file.
class LocalServiceDiscovery : public IServiceDiscovery {
public:
    LocalServiceDiscovery(std::chrono::seconds cleanup_interval = std::chrono::seconds(300), const std::string& persistence_file_path = "");
    ~LocalServiceDiscovery();

    // Non-copyable
    LocalServiceDiscovery(const LocalServiceDiscovery&) = delete;
    LocalServiceDiscovery& operator=(const LocalServiceDiscovery&) = delete;

    bool register_service(const ServiceInstance& instance, std::optional<std::chrono::seconds> ttl = std::nullopt) override;
    bool deregister_service(const std::string& service_name, const std::string& instance_id) override;
    std::optional<ServiceInstance> query_service(const std::string& service_name) override;
    std::vector<ServiceInstance> query_all_services(const std::string& service_name) override;
    std::vector<ServiceInstance> query_services_by_metadata(const std::map<std::string, std::string>& metadata_filters) override;

    std::vector<ServiceInstance> query_services_by_criteria(
        const std::string& service_name,
        const std::string& version_filter = "",
        const std::string& region_filter = "",
        const std::string& environment_filter = "",
        const std::vector<std::string>& required_tags = {}) override;

private:
    mutable std::shared_mutex _mutex;
    // Maps a service name to a map of its registered instances, keyed by instance_id.
    std::map<std::string, std::unordered_map<std::string, ServiceInstance>> _services;

    std::thread _cleanup_thread;
    std::atomic<bool> _running_cleanup{false};
    std::condition_variable _cleanup_cv;
    std::mutex _cleanup_mutex;

    // For random number generation
    std::mt19937 _random_gen;
    mutable std::mutex _random_gen_mutex;

    std::chrono::seconds _cleanup_interval;
    std::string _persistence_file_path;

    void _cleanup_loop();
    void _save_services_to_file();
    void _load_services_from_file();
};

/// @brief Factory function to create an instance of InMemoryServiceDiscovery.
std::unique_ptr<IServiceDiscovery> make_local_discovery(std::chrono::seconds cleanup_interval = std::chrono::seconds(300), const std::string& persistence_file_path = "");

} // namespace shield::discovery
