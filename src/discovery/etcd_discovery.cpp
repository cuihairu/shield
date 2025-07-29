// shield/src/discovery/etcd_discovery.cpp
#include "shield/discovery/etcd_discovery.hpp"
#include "shield/core/logger.hpp"
#include "nlohmann/json.hpp"

#include <etcd/Client.hpp>
#include <etcd/SyncClient.hpp>
#include <etcd/KeepAlive.hpp>

#include <memory>
#include <thread>
#include <map>
#include <mutex>
#include <random>

namespace shield::discovery {

// Helper to create the etcd key
static std::string make_key(const std::string& service_name, const std::string& instance_id) {
    return "/services/" + service_name + "/" + instance_id;
}


// PIMPL idiom to hide etcd client details
class EtcdServiceDiscovery::EtcdDiscoveryImpl {
public:
    explicit EtcdDiscoveryImpl(const std::string& endpoints)
        : _client(endpoints), _sync_client(endpoints) {
        SHIELD_LOG_INFO << "EtcdServiceDiscovery initialized with endpoints: " << endpoints;
    }

    ~EtcdDiscoveryImpl() {
        std::lock_guard<std::mutex> lock(_leases_mutex);
        for (auto const& [instance_id, keepalive] : _leases) {
            if (keepalive) {
                keepalive->Cancel();
            }
        }
        _leases.clear();
    }

    bool register_service(const ServiceInstance& instance, std::optional<std::chrono::seconds> ttl) {
        if (instance.service_name.empty() || instance.instance_id.empty()) {
            SHIELD_LOG_ERROR << "Service name and instance ID cannot be empty.";
            return false;
        }

        long lease_ttl = ttl.has_value() ? ttl->count() : 60; // Default to 60s TTL
        etcd::Response resp = _sync_client.leasegrant(lease_ttl);

        if (!resp.is_ok()) {
            SHIELD_LOG_ERROR << "Failed to grant etcd lease: " << resp.error_message();
            return false;
        }

        int64_t lease_id = resp.value().lease();
        auto key = make_key(instance.service_name, instance.instance_id);
        nlohmann::json instance_json = instance;
        
        resp = _sync_client.put(key, instance_json.dump(), lease_id);
        if (!resp.is_ok()) {
            SHIELD_LOG_ERROR << "Failed to put service key to etcd: " << resp.error_message();
            _sync_client.leaserevoke(lease_id);
            return false;
        }

        try {
            // leasekeepalive returns a task, we need to get the result
            auto keepalive = _client.leasekeepalive(lease_id).get();
            std::lock_guard<std::mutex> lock(_leases_mutex);
            _leases[instance.instance_id] = std::move(keepalive);
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Failed to start lease keep-alive: " << e.what();
            _sync_client.leaserevoke(lease_id);
            return false;
        }

        SHIELD_LOG_INFO << "Registered service '" << instance.service_name << "' with ID '" << instance.instance_id << "' on lease " << lease_id;
        return true;
    }

    bool deregister_service(const std::string& service_name, const std::string& instance_id) {
        std::shared_ptr<etcd::KeepAlive> keepalive_to_cancel;
        {
            std::lock_guard<std::mutex> lock(_leases_mutex);
            auto it = _leases.find(instance_id);
            if (it != _leases.end()) {
                keepalive_to_cancel = std::move(it->second);
                _leases.erase(it);
            }
        }

        if (keepalive_to_cancel) {
            keepalive_to_cancel->Cancel();
            SHIELD_LOG_INFO << "Deregistered service with ID '" << instance_id << "' by revoking lease.";
            return true;
        } else {
            auto key = make_key(service_name, instance_id);
            etcd::Response resp = _sync_client.rm(key);
            if (resp.is_ok()) {
                SHIELD_LOG_INFO << "Deregistered service with ID '" << instance_id << "' by deleting key.";
                return true;
            }
            SHIELD_LOG_WARN << "Could not find lease for instance ID '" << instance_id << "' and failed to delete key: " << resp.error_message();
            return false;
        }
    }

    std::vector<ServiceInstance> query_all_services(const std::string& service_name) {
        std::vector<ServiceInstance> instances;
        auto prefix = "/services/" + service_name + "/";
        
        std::string range_end = prefix;
        if (!prefix.empty()) {
            range_end.back() = static_cast<char>(range_end.back() + 1);
        }

        etcd::Response resp = _sync_client.ls(prefix, range_end);

        if (!resp.is_ok()) {
            SHIELD_LOG_ERROR << "Failed to query etcd with prefix " << prefix << ": " << resp.error_message();
            return instances;
        }

        for (const auto& val : resp.values()) {
            try {
                ServiceInstance instance = nlohmann::json::parse(val.as_string());
                instances.push_back(instance);
            } catch (const std::exception& e) {
                SHIELD_LOG_ERROR << "Failed to parse service instance JSON for key " << val.key() << ": " << e.what();
            }
        }
        return instances;
    }

private:
    etcd::Client _client;
    etcd::SyncClient _sync_client;
    std::map<std::string, std::shared_ptr<etcd::KeepAlive>> _leases;
    std::mutex _leases_mutex;
};

// --- Public Interface Implementation ---

EtcdServiceDiscovery::EtcdServiceDiscovery(const std::string& etcd_endpoints)
    : _impl(std::make_unique<EtcdDiscoveryImpl>(etcd_endpoints)) {}

EtcdServiceDiscovery::~EtcdServiceDiscovery() = default;

bool EtcdServiceDiscovery::register_service(const ServiceInstance& instance, std::optional<std::chrono::seconds> ttl) {
    return _impl->register_service(instance, ttl);
}

bool EtcdServiceDiscovery::deregister_service(const std::string& service_name, const std::string& instance_id) {
    return _impl->deregister_service(service_name, instance_id);
}

std::vector<ServiceInstance> EtcdServiceDiscovery::query_all_services(const std::string& service_name) {
    return _impl->query_all_services(service_name);
}

std::optional<ServiceInstance> EtcdServiceDiscovery::query_service(const std::string& service_name) {
    auto instances = query_all_services(service_name);
    if (instances.empty()) {
        return std::nullopt;
    }
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, instances.size() - 1);
    return instances[distrib(gen)];
}

std::vector<ServiceInstance> EtcdServiceDiscovery::query_services_by_metadata(const std::map<std::string, std::string>& metadata_filters) {
    auto service_name_it = metadata_filters.find("service_name");
    if (service_name_it == metadata_filters.end()) {
        SHIELD_LOG_ERROR << "etcd query_services_by_metadata requires a 'service_name' in filters.";
        return {};
    }
    
    auto all_instances = query_all_services(service_name_it->second);
    std::vector<ServiceInstance> matching_instances;

    for (const auto& instance : all_instances) {
        bool matches_all = true;
        
        for (const auto& filter : metadata_filters) {
            if (filter.first == "service_name") continue;
            
            if (!instance.metadata.matches_filters({{filter.first, filter.second}})) {
                matches_all = false;
                break;
            }
        }

        if (matches_all) {
            matching_instances.push_back(instance);
        }
    }
    return matching_instances;
}

std::vector<ServiceInstance> EtcdServiceDiscovery::query_services_by_criteria(
    const std::string& service_name,
    const std::string& version_filter,
    const std::string& region_filter,  
    const std::string& environment_filter,
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
        if (!environment_filter.empty() && metadata.environment != environment_filter) {
            continue;
        }
        
        // Check required tags
        bool has_all_tags = true;
        for (const auto& required_tag : required_tags) {
            if (std::find(metadata.tags.begin(), metadata.tags.end(), required_tag) == metadata.tags.end()) {
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


// --- Factory Function ---

std::unique_ptr<IServiceDiscovery> make_etcd_discovery(const std::string& etcd_endpoints) {
    return std::make_unique<EtcdServiceDiscovery>(etcd_endpoints);
}

} // namespace shield::discovery
