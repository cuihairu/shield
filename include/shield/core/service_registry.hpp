// [SHIELD_CORE] Service registry for local naming
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shield::core {

class ServiceHandle;
class ServiceContext;

/// @brief Local service registry for naming and lookup
/// Note: Cross-node discovery is NOT part of core (see shield_cluster)
class ServiceRegistry {
public:
    ServiceRegistry();
    ~ServiceRegistry();

    // Non-copyable
    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;

    // Register a service with a unique name
    /// @param service_name Unique name for this service
    /// @param handle Handle to the service
    /// @return true if registration succeeded, false if name already taken
    bool register_service(std::string_view service_name, ServiceHandle handle);

    // Unregister a service by name
    /// @param service_name Name of the service to unregister
    /// @return true if service was found and removed
    bool unregister_service(std::string_view service_name);

    // Find a service by name
    /// @param service_name Name of the service to find
    /// @return Handle if found, invalid handle otherwise
    ServiceHandle find_service(std::string_view service_name) const;

    // List all registered service names
    std::vector<std::string> list_services() const;

    // Check if a service exists
    bool has_service(std::string_view service_name) const;

    // Get count of registered services
    size_t size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace shield::core
