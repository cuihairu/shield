// shield/include/shield/discovery/service_discovery.hpp
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include "shield/discovery/service_instance.hpp"

namespace shield::discovery {

/// @brief Defines the contract for any service discovery implementation.
/// This interface allows decoupling business logic components from the concrete
/// service discovery technology (e.g., etcd, Nacos, Consul).
class IServiceDiscovery {
public:
    virtual ~IServiceDiscovery() = default;

    /// @brief Registers a service instance with the discovery backend.
    /// This method should also handle service health checks (e.g.,
    /// heartbeating).
    /// @param instance The service instance to register.
    /// @return True on success, false otherwise.
    virtual bool register_service(
        const ServiceInstance &instance,
        std::optional<std::chrono::seconds> ttl = std::nullopt) = 0;

    /// @brief Deregisters a service instance.
    /// @param instance_id The unique ID of the instance to deregister.
    /// @return True on success, false otherwise.
    virtual bool deregister_service(const std::string &service_name,
                                    const std::string &instance_id) = 0;

    /// @brief Queries for a single, healthy instance of a named service.
    /// If multiple instances exist, the implementation should apply a
    /// load-balancing strategy (e.g., random, round-robin) to return one.
    /// @param service_name The name of the service to query.
    /// @return A ServiceInstance if found, otherwise std::nullopt.
    virtual std::optional<ServiceInstance> query_service(
        const std::string &service_name) = 0;

    /// @brief Queries for all healthy instances of a named service.
    /// @param service_name The name of the service to query.
    /// @return A vector of all available service instances.
    virtual std::vector<ServiceInstance> query_all_services(
        const std::string &service_name) = 0;

    /// @brief Queries for service instances matching specific metadata filters.
    /// @param metadata_filters A map of metadata key-value pairs to filter by.
    /// @return A vector of matching service instances.
    virtual std::vector<ServiceInstance> query_services_by_metadata(
        const std::map<std::string, std::string> &metadata_filters) = 0;

    /// @brief Queries for service instances matching structured metadata
    /// criteria.
    /// @param service_name The name of the service to query.
    /// @param version_filter Optional version filter (empty means any version).
    /// @param region_filter Optional region filter (empty means any region).
    /// @param environment_filter Optional environment filter (empty means any
    /// environment).
    /// @param required_tags Optional list of tags that must be present.
    /// @return A vector of matching service instances.
    virtual std::vector<ServiceInstance> query_services_by_criteria(
        const std::string &service_name, const std::string &version_filter = "",
        const std::string &region_filter = "",
        const std::string &environment_filter = "",
        const std::vector<std::string> &required_tags = {}) = 0;
};

}  // namespace shield::discovery
