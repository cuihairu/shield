// shield/include/shield/discovery/redis_discovery.hpp
#pragma once

#include "shield/discovery/service_discovery.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Forward declare Redis client to avoid including heavy headers in public API
namespace sw::redis {
class Redis;
}

namespace shield::discovery {

/// @brief An implementation of IServiceDiscovery using Redis as a backend.
/// This implementation supports TTL-based service expiration and provides
/// a basic heartbeating mechanism for registered services.
class RedisServiceDiscovery : public IServiceDiscovery {
public:
  /// @brief Constructs a RedisServiceDiscovery instance.
  /// @param redis_uri The Redis connection URI, e.g., "tcp://127.0.0.1:6379".
  /// @param heartbeat_interval The interval at which registered services are
  /// heartbeated (renewed).
  RedisServiceDiscovery(
      const std::string &redis_uri,
      std::chrono::seconds heartbeat_interval = std::chrono::seconds(5));
  ~RedisServiceDiscovery() override;

  // Non-copyable
  RedisServiceDiscovery(const RedisServiceDiscovery &) = delete;
  RedisServiceDiscovery &operator=(const RedisServiceDiscovery &) = delete;

  bool register_service(
      const ServiceInstance &instance,
      std::optional<std::chrono::seconds> ttl = std::nullopt) override;
  bool deregister_service(const std::string &service_name,
                          const std::string &instance_id) override;
  std::optional<ServiceInstance>
  query_service(const std::string &service_name) override;
  std::vector<ServiceInstance>
  query_all_services(const std::string &service_name) override;
  std::vector<ServiceInstance> query_services_by_metadata(
      const std::map<std::string, std::string> &metadata_filters) override;
  std::vector<ServiceInstance> query_services_by_criteria(
      const std::string &service_name, const std::string &version_filter = "",
      const std::string &region_filter = "",
      const std::string &environment_filter = "",
      const std::vector<std::string> &required_tags = {}) override;

private:
  class RedisDiscoveryImpl;
  std::unique_ptr<RedisDiscoveryImpl> _impl;
};

/// @brief Factory function to create an instance of RedisServiceDiscovery.
/// @param redis_uri The Redis connection URI, e.g., "tcp://127.0.0.1:6379".
/// @param heartbeat_interval The interval at which registered services are
/// heartbeated (renewed).
std::unique_ptr<IServiceDiscovery> make_redis_discovery(
    const std::string &redis_uri,
    std::chrono::seconds heartbeat_interval = std::chrono::seconds(5));

} // namespace shield::discovery
