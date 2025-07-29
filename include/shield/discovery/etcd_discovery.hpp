// shield/include/shield/discovery/etcd_discovery.hpp
#pragma once

#include "shield/discovery/service_discovery.hpp"
#include <memory>

// Forward-declare the etcd client class to avoid including etcd headers here.
// This reduces compile-time dependencies.
namespace etcd {
class Client;
}

namespace shield::discovery {

/// @brief An implementation of IServiceDiscovery using an etcd v3 backend.
class EtcdServiceDiscovery : public IServiceDiscovery {
public:
  /// @param etcd_endpoints A comma-separated list of etcd server endpoints,
  /// e.g., "http://127.0.0.1:2379".
  explicit EtcdServiceDiscovery(const std::string &etcd_endpoints);
  ~EtcdServiceDiscovery() override;

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
  // Forward declaration of the implementation class (PIMPL idiom)
  class EtcdDiscoveryImpl;
  std::unique_ptr<EtcdDiscoveryImpl> _impl;
};

/// @brief Factory function to create an instance of EtcdServiceDiscovery.
/// @param etcd_endpoints A comma-separated list of etcd server endpoints.
/// @return A unique_ptr to the IServiceDiscovery interface.
std::unique_ptr<IServiceDiscovery>
make_etcd_discovery(const std::string &etcd_endpoints);

} // namespace shield::discovery
