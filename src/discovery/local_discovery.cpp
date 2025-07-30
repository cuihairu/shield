// shield/src/discovery/in_memory_discovery.cpp
#include "shield/discovery/local_discovery.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

#include "nlohmann/json.hpp"
#include "shield/log/logger.hpp"

namespace shield::discovery {

LocalServiceDiscovery::LocalServiceDiscovery(
    std::chrono::seconds cleanup_interval,
    const std::string& persistence_file_path)
    : _running_cleanup(true),
      _cleanup_interval(cleanup_interval),
      _persistence_file_path(persistence_file_path) {
    std::random_device rd;
    _random_gen.seed(rd());

    if (!_persistence_file_path.empty()) {
        _load_services_from_file();
    }

    _cleanup_thread = std::thread(&LocalServiceDiscovery::_cleanup_loop, this);
}

LocalServiceDiscovery::~LocalServiceDiscovery() {
    _running_cleanup = false;
    _cleanup_cv.notify_one();  // Notify cleanup thread to stop
    if (_cleanup_thread.joinable()) {
        _cleanup_thread.join();
    }
    // Perform a final save on shutdown if persistence is enabled
    if (!_persistence_file_path.empty()) {
        _save_services_to_file();
    }
}

bool LocalServiceDiscovery::register_service(
    const ServiceInstance& instance_param,
    std::optional<std::chrono::seconds> ttl) {
    if (instance_param.service_name.empty() ||
        instance_param.instance_id.empty()) {
        return false;  // Cannot register a service with an empty name or ID
    }

    ServiceInstance instance = instance_param;  // Create a mutable copy

    if (ttl) {
        instance.expiration_time =
            std::chrono::steady_clock::now() + ttl.value();
    } else {
        // Set to a very distant future for services that don't expire
        // automatically
        instance.expiration_time = std::chrono::steady_clock::time_point::max();
    }

    std::unique_lock<std::shared_mutex> lock(_mutex);
    _services[instance.service_name][instance.instance_id] = instance;
    return true;
}

bool LocalServiceDiscovery::deregister_service(const std::string& service_name,
                                               const std::string& instance_id) {
    if (service_name.empty() || instance_id.empty()) {
        return false;  // Cannot deregister a service with an empty name or ID
    }
    std::unique_lock<std::shared_mutex> lock(_mutex);
    auto it = _services.find(service_name);
    if (it != _services.end()) {
        it->second.erase(instance_id);
        if (it->second.empty()) {
            _services.erase(
                it);  // Remove service_name entry if no instances left
        }
    }
    return true;
}

std::optional<ServiceInstance> LocalServiceDiscovery::query_service(
    const std::string& service_name) {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto it = _services.find(service_name);
    if (it == _services.end() || it->second.empty()) {
        return std::nullopt;
    }

    std::vector<ServiceInstance> available_instances;
    auto now = std::chrono::steady_clock::now();
    for (const auto& pair : it->second) {
        if (pair.second.expiration_time > now) {
            available_instances.push_back(pair.second);
        }
    }

    if (available_instances.empty()) {
        return std::nullopt;
    }

    // Simple random load balancing
    std::unique_lock<std::mutex> random_lock(_random_gen_mutex);
    std::uniform_int_distribution<> distrib(0, available_instances.size() - 1);
    return available_instances[distrib(_random_gen)];
}

std::vector<ServiceInstance> LocalServiceDiscovery::query_all_services(
    const std::string& service_name) {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto it = _services.find(service_name);
    if (it == _services.end()) {
        return {};
    }
    std::vector<ServiceInstance> instances;
    auto now = std::chrono::steady_clock::now();
    for (const auto& pair : it->second) {
        if (pair.second.expiration_time > now) {
            instances.push_back(pair.second);
        }
    }
    return instances;
}

std::vector<ServiceInstance> LocalServiceDiscovery::query_services_by_metadata(
    const std::map<std::string, std::string>& metadata_filters) {
    std::vector<ServiceInstance> matching_instances;
    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto now = std::chrono::steady_clock::now();

    for (const auto& service_pair : _services) {
        for (const auto& instance_pair : service_pair.second) {
            const ServiceInstance& instance = instance_pair.second;
            if (instance.expiration_time <= now) {
                continue;  // Skip expired instances
            }

            bool matches_all_filters = true;

            for (const auto& filter_pair : metadata_filters) {
                if (!instance.metadata.matches_filters(
                        {{filter_pair.first, filter_pair.second}})) {
                    matches_all_filters = false;
                    break;
                }
            }

            if (matches_all_filters) {
                matching_instances.push_back(instance);
            }
        }
    }
    return matching_instances;
}

std::vector<ServiceInstance> LocalServiceDiscovery::query_services_by_criteria(
    const std::string& service_name, const std::string& version_filter,
    const std::string& region_filter, const std::string& environment_filter,
    const std::vector<std::string>& required_tags) {
    auto all_instances = query_all_services(service_name);
    std::vector<ServiceInstance> matching_instances;

    for (const auto& instance : all_instances) {
        const auto& metadata = instance.metadata;

        // Check version filter
        if (!version_filter.empty() && metadata.version != version_filter) {
            continue;
        }

        // Check region filter
        if (!region_filter.empty() && metadata.region != region_filter) {
            continue;
        }

        // Check environment filter
        if (!environment_filter.empty() &&
            metadata.environment != environment_filter) {
            continue;
        }

        // Check required tags
        bool has_all_tags = true;
        for (const auto& required_tag : required_tags) {
            if (std::find(metadata.tags.begin(), metadata.tags.end(),
                          required_tag) == metadata.tags.end()) {
                has_all_tags = false;
                break;
            }
        }

        if (has_all_tags) {
            matching_instances.push_back(instance);
        }
    }

    return matching_instances;
}

void LocalServiceDiscovery::_cleanup_loop() {
    while (_running_cleanup) {
        std::unique_lock<std::mutex> lock(_cleanup_mutex);
        _cleanup_cv.wait_for(lock, _cleanup_interval,
                             [this] { return !_running_cleanup; });

        if (!_running_cleanup) {
            break;  // Exit if signaled to stop
        }

        std::unique_lock<std::shared_mutex> services_lock(_mutex);
        auto now = std::chrono::steady_clock::now();

        for (auto service_it = _services.begin();
             service_it != _services.end();) {
            auto& instances_map = service_it->second;
            for (auto instance_it = instances_map.begin();
                 instance_it != instances_map.end();) {
                if (instance_it->second.expiration_time <= now) {
                    instance_it = instances_map.erase(instance_it);
                } else {
                    ++instance_it;
                }
            }
            if (instances_map.empty()) {
                service_it = _services.erase(service_it);
            } else {
                ++service_it;
            }
        }
        services_lock.unlock();  // Release lock before saving to file

        if (!_persistence_file_path.empty()) {
            _save_services_to_file();
        }
    }
}

void LocalServiceDiscovery::_save_services_to_file() {
    std::shared_lock<std::shared_mutex> lock(
        _mutex);  // Read lock for _services
    nlohmann::json j;
    for (const auto& service_pair : _services) {
        nlohmann::json instances_array = nlohmann::json::array();
        for (const auto& instance_pair : service_pair.second) {
            instances_array.push_back(instance_pair.second);
        }
        j[service_pair.first] = instances_array;
    }

    // Atomic write: write to a temporary file, then rename
    std::filesystem::path file_path(_persistence_file_path);
    std::filesystem::path temp_file_path = file_path.string() + ".tmp";

    // Ensure directory exists
    try {
        std::filesystem::create_directories(file_path.parent_path());
    } catch (const std::filesystem::filesystem_error& e) {
        SHIELD_LOG_ERROR << "Failed to create directories for persistence file "
                         << file_path.parent_path() << ": " << e.what();
        return;
    }

    std::ofstream ofs(temp_file_path);
    if (ofs.is_open()) {
        ofs << j.dump(4);  // Pretty print with 4 spaces indentation
        ofs.close();
        try {
            std::filesystem::rename(temp_file_path, file_path);
        } catch (const std::filesystem::filesystem_error& e) {
            SHIELD_LOG_ERROR << "Failed to rename temporary file "
                             << temp_file_path << " to " << file_path << ": "
                             << e.what();
        }
    } else {
        SHIELD_LOG_ERROR << "Could not open temporary file " << temp_file_path
                         << " for writing.";
    }
}

void LocalServiceDiscovery::_load_services_from_file() {
    std::unique_lock<std::shared_mutex> lock(
        _mutex);  // Write lock for _services
    std::ifstream ifs(_persistence_file_path);
    if (ifs.is_open()) {
        try {
            nlohmann::json j = nlohmann::json::parse(ifs);
            _services.clear();  // Clear current services before loading
            for (auto it = j.begin(); it != j.end(); ++it) {
                std::string service_name = it.key();
                for (const auto& instance_json : it.value()) {
                    ServiceInstance instance =
                        instance_json.get<ServiceInstance>();
                    _services[service_name][instance.instance_id] = instance;
                }
            }
        } catch (const nlohmann::json::parse_error& e) {
            SHIELD_LOG_ERROR
                << "JSON parse error when loading persistence file "
                << _persistence_file_path << ": " << e.what();
        } catch (const nlohmann::json::exception& e) {
            SHIELD_LOG_ERROR << "JSON exception when loading persistence file "
                             << _persistence_file_path << ": " << e.what();
        }
        ifs.close();
    } else {
        SHIELD_LOG_WARN
            << "Persistence file not found or could not be opened for reading: "
            << _persistence_file_path
            << ". Starting with empty service registry.";
    }
}

std::unique_ptr<IServiceDiscovery> make_local_discovery(
    std::chrono::seconds cleanup_interval,
    const std::string& persistence_file_path) {
    return std::make_unique<LocalServiceDiscovery>(cleanup_interval,
                                                   persistence_file_path);
}

}  // namespace shield::discovery
