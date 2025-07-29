// shield/src/discovery/service_instance.cpp
#include "shield/discovery/service_instance.hpp"

#include <algorithm>

namespace shield::discovery {

// ServiceMetadata implementation
bool ServiceMetadata::matches_filters(
    const std::map<std::string, std::string>& filters) const {
    for (const auto& [key, value] : filters) {
        if (key == "version") {
            if (!version.empty() && version != value) {
                return false;
            }
            continue;
        }

        if (key == "region") {
            if (!region.empty() && region != value) {
                return false;
            }
            continue;
        }

        if (key == "environment") {
            if (!environment.empty() && environment != value) {
                return false;
            }
            continue;
        }

        if (key == "weight") {
            if (std::to_string(weight) != value) {
                return false;
            }
            continue;
        }

        if (key == "tag") {
            if (std::find(tags.begin(), tags.end(), value) == tags.end()) {
                return false;
            }
            continue;
        }

        // Check custom attributes
        auto attr_it = custom_attributes.find(key);
        if (attr_it != custom_attributes.end()) {
            if (attr_it->second != value) {
                return false;
            }
            continue;
        }

        // If key is not found in any category, it's a mismatch
        return false;
    }

    return true;
}

std::string ServiceMetadata::to_json() const {
    nlohmann::json j;
    j["version"] = version;
    j["region"] = region;
    j["environment"] = environment;
    j["weight"] = weight;
    j["tags"] = tags;
    j["custom_attributes"] = custom_attributes;
    return j.dump();
}

ServiceMetadata ServiceMetadata::from_json(const std::string& json_str) {
    ServiceMetadata metadata;

    if (json_str.empty()) {
        return metadata;
    }

    try {
        auto j = nlohmann::json::parse(json_str);

        if (j.contains("version")) {
            metadata.version = j.at("version").get<std::string>();
        }
        if (j.contains("region")) {
            metadata.region = j.at("region").get<std::string>();
        }
        if (j.contains("environment")) {
            metadata.environment = j.at("environment").get<std::string>();
        }
        if (j.contains("weight")) {
            metadata.weight = j.at("weight").get<uint32_t>();
        }
        if (j.contains("tags")) {
            metadata.tags = j.at("tags").get<std::vector<std::string>>();
        }
        if (j.contains("custom_attributes")) {
            metadata.custom_attributes =
                j.at("custom_attributes")
                    .get<std::map<std::string, std::string>>();
        }
    } catch (const std::exception&) {
        // Return default metadata on parse error
    }

    return metadata;
}

// ServiceInstance JSON serialization
void to_json(nlohmann::json& j, const ServiceInstance& p) {
    j = nlohmann::json{{"service_name", p.service_name},
                       {"instance_id", p.instance_id},
                       {"address", p.address},
                       {"metadata", p.metadata.to_json()}};
    // Only include expiration_time if it's not max (i.e., has a meaningful
    // value)
    if (p.expiration_time != std::chrono::steady_clock::time_point::max()) {
        j["expiration_time"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                p.expiration_time.time_since_epoch())
                .count();
    }
}

void from_json(const nlohmann::json& j, ServiceInstance& p) {
    j.at("service_name").get_to(p.service_name);
    j.at("instance_id").get_to(p.instance_id);
    j.at("address").get_to(p.address);

    if (j.contains("metadata")) {
        p.metadata =
            ServiceMetadata::from_json(j.at("metadata").get<std::string>());
    }

    if (j.contains("expiration_time")) {
        auto duration =
            std::chrono::milliseconds(j.at("expiration_time").get<int64_t>());
        p.expiration_time = std::chrono::steady_clock::time_point(duration);
    } else {
        p.expiration_time = std::chrono::steady_clock::time_point::max();
    }
}

}  // namespace shield::discovery