// shield/include/shield/discovery/service_instance.hpp
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace shield::discovery {

/// @brief Service metadata with well-defined structure for internal use
struct ServiceMetadata {
    /// @brief Service version (e.g., "1.2.0")
    std::string version;
    /// @brief Deployment region (e.g., "us-west-1", "local")
    std::string region;
    /// @brief Environment (e.g., "prod", "staging", "dev")
    std::string environment;
    /// @brief Load balancing weight (1-100)
    uint32_t weight = 100;
    /// @brief Service tags for filtering
    std::vector<std::string> tags;
    /// @brief Additional custom attributes
    std::map<std::string, std::string> custom_attributes;

    /// @brief Check if this metadata matches the given filters
    bool matches_filters(
        const std::map<std::string, std::string> &filters) const;

    /// @brief Convert to JSON string for storage
    std::string to_json() const;

    /// @brief Create from JSON string
    static ServiceMetadata from_json(const std::string &json_str);
};

/// @brief Holds all necessary information about a single instance of a service.
struct ServiceInstance {
    /// @brief The logical name of the service, e.g., "auth-service".
    std::string service_name;
    /// @brief A unique identifier for this specific instance.
    std::string instance_id;
    /// @brief The CAF actor URI string that other services can use to connect.
    std::string address;
    /// @brief Structured metadata for the service instance
    ServiceMetadata metadata;
    /// @brief The time point when this service instance is considered expired.
    /// If not set, or set to max(), it means the service does not expire
    /// automatically.
    std::chrono::steady_clock::time_point expiration_time;
};

// JSON serialization functions for ServiceInstance
void to_json(nlohmann::json &j, const ServiceInstance &p);
void from_json(const nlohmann::json &j, ServiceInstance &p);

}  // namespace shield::discovery