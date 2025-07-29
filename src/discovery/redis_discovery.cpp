// shield/src/discovery/redis_discovery.cpp
#include "shield/discovery/redis_discovery.hpp"
#include "shield/core/logger.hpp"

#include <sw/redis++/redis++.h>

#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
#include <map>
#include <vector>
#include <random>
#include <algorithm>

namespace shield::discovery {

// Helper to create the Redis key for the service hash
static std::string make_service_key(const std::string& service_name) {
    return "services:" + service_name;
}

// Helper to create the Redis key for the instance TTL
static std::string make_ttl_key(const std::string& service_name, const std::string& instance_id) {
    return "services:ttl:" + service_name + ":" + instance_id;
}

// PIMPL idiom
class RedisServiceDiscovery::RedisDiscoveryImpl {
public:
    explicit RedisDiscoveryImpl(const std::string& redis_uri, std::chrono::seconds heartbeat_interval)
        : _redis(redis_uri), _heartbeat_interval(heartbeat_interval), _running_heartbeat(true) {
        SHIELD_LOG_INFO << "RedisServiceDiscovery initialized with URI: " << redis_uri;
        _heartbeat_thread = std::thread(&RedisDiscoveryImpl::_heartbeat_loop, this);
    }

    ~RedisDiscoveryImpl() {
        _running_heartbeat = false;
        _heartbeat_cv.notify_one();
        if (_heartbeat_thread.joinable()) {
            _heartbeat_thread.join();
        }
    }

    bool register_service(const ServiceInstance& instance, std::optional<std::chrono::seconds> ttl) {
        if (instance.service_name.empty() || instance.instance_id.empty()) {
            SHIELD_LOG_ERROR << "Service name and instance ID cannot be empty.";
            return false;
        }

        try {
            auto service_key = make_service_key(instance.service_name);
            auto ttl_key = make_ttl_key(instance.service_name, instance.instance_id);
            long ttl_seconds = ttl.has_value() ? ttl->count() : 60; // Default TTL 60s

            nlohmann::json instance_json = instance;

            auto pipe = _redis.pipeline();
            pipe.hset(service_key, instance.instance_id, instance_json.dump());
            pipe.setex(ttl_key, ttl_seconds, "1");
            pipe.exec();

            // Add to heartbeat list
            std::lock_guard<std::mutex> lock(_heartbeat_mutex);
            _heartbeat_keys[ttl_key] = ttl_seconds;

            SHIELD_LOG_INFO << "Registered service '" << instance.service_name << "' with ID '" << instance.instance_id << "'.";
            return true;
        } catch (const sw::redis::Error& e) {
            SHIELD_LOG_ERROR << "Redis error on register_service: " << e.what();
            return false;
        }
    }

    bool deregister_service(const std::string& service_name, const std::string& instance_id) {
        try {
            auto service_key = make_service_key(service_name);
            auto ttl_key = make_ttl_key(service_name, instance_id);

            // Remove from heartbeat list
            {
                std::lock_guard<std::mutex> lock(_heartbeat_mutex);
                _heartbeat_keys.erase(ttl_key);
            }

            auto pipe = _redis.pipeline();
            pipe.hdel(service_key, instance_id);
            pipe.del(ttl_key);
            pipe.exec();

            SHIELD_LOG_INFO << "Deregistered service '" << service_name << "' with ID '" << instance_id << "'.";
            return true;
        } catch (const sw::redis::Error& e) {
            SHIELD_LOG_ERROR << "Redis error on deregister_service: " << e.what();
            return false;
        }
    }

    std::vector<ServiceInstance> query_all_services(const std::string& service_name) {
        std::vector<ServiceInstance> instances;
        try {
            auto service_key = make_service_key(service_name);
            std::vector<std::pair<std::string, std::string>> all_instances_map;
            _redis.hgetall(service_key, std::back_inserter(all_instances_map));

            if (all_instances_map.empty()) {
                return instances;
            }

            // Check for health (existence of TTL key)
            auto pipe = _redis.pipeline();
            std::vector<std::string> instance_ids;
            for (const auto& pair : all_instances_map) {
                instance_ids.push_back(pair.first);
                pipe.exists(make_ttl_key(service_name, pair.first));
            }
            auto replies = pipe.exec();

            for (size_t i = 0; i < all_instances_map.size(); ++i) {
                if (replies.get<long long>(i) > 0) { // Key exists, so it's healthy
                    try {
                        ServiceInstance instance = nlohmann::json::parse(all_instances_map[i].second);
                        instances.push_back(instance);
                    } catch (const std::exception& e) {
                        SHIELD_LOG_ERROR << "Failed to parse service instance JSON for " << all_instances_map[i].first << ": " << e.what();
                    }
                }
            }
        } catch (const sw::redis::Error& e) {
            SHIELD_LOG_ERROR << "Redis error on query_all_services: " << e.what();
        }
        return instances;
    }

private:
    void _heartbeat_loop() {
        while (_running_heartbeat) {
            std::unique_lock<std::mutex> lock(_heartbeat_cv_mutex);
            _heartbeat_cv.wait_for(lock, _heartbeat_interval, [this] { return !_running_heartbeat; });

            if (!_running_heartbeat) break;

            try {
                std::lock_guard<std::mutex> hb_lock(_heartbeat_mutex);
                if (_heartbeat_keys.empty()) continue;

                auto pipe = _redis.pipeline();
                for (const auto& pair : _heartbeat_keys) {
                    pipe.expire(pair.first, pair.second);
                }
                pipe.exec();
            } catch (const sw::redis::Error& e) {
                SHIELD_LOG_ERROR << "Redis heartbeat failed: " << e.what();
            }
        }
    }

    sw::redis::Redis _redis;
    std::chrono::seconds _heartbeat_interval;
    bool _running_heartbeat;
    std::thread _heartbeat_thread;
    std::mutex _heartbeat_cv_mutex;
    std::condition_variable _heartbeat_cv;

    std::map<std::string, long> _heartbeat_keys;
    std::mutex _heartbeat_mutex;
};

// --- Public Interface Implementation ---

RedisServiceDiscovery::RedisServiceDiscovery(const std::string& redis_uri, std::chrono::seconds heartbeat_interval)
    : _impl(std::make_unique<RedisDiscoveryImpl>(redis_uri, heartbeat_interval)) {}

RedisServiceDiscovery::~RedisServiceDiscovery() = default;

bool RedisServiceDiscovery::register_service(const ServiceInstance& instance, std::optional<std::chrono::seconds> ttl) {
    return _impl->register_service(instance, ttl);
}

bool RedisServiceDiscovery::deregister_service(const std::string& service_name, const std::string& instance_id) {
    return _impl->deregister_service(service_name, instance_id);
}

std::vector<ServiceInstance> RedisServiceDiscovery::query_all_services(const std::string& service_name) {
    return _impl->query_all_services(service_name);
}

std::optional<ServiceInstance> RedisServiceDiscovery::query_service(const std::string& service_name) {
    auto instances = query_all_services(service_name);
    if (instances.empty()) {
        return std::nullopt;
    }
    // Simple random load balancing
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, instances.size() - 1);
    return instances[distrib(gen)];
}

std::vector<ServiceInstance> RedisServiceDiscovery::query_services_by_metadata(const std::map<std::string, std::string>& metadata_filters) {
    auto service_name_it = metadata_filters.find("service_name");
    if (service_name_it == metadata_filters.end()) {
        SHIELD_LOG_ERROR << "Redis query_services_by_metadata requires a 'service_name' in filters.";
        return {};
    }

    auto all_instances = query_all_services(service_name_it->second);
    std::vector<ServiceInstance> matching_instances;

    // Filter using structured metadata
    for (const auto& instance : all_instances) {
        if (instance.metadata.matches_filters(metadata_filters)) {
            matching_instances.push_back(instance);
        }
    }
    return matching_instances;
}

std::vector<ServiceInstance> RedisServiceDiscovery::query_services_by_criteria(
    const std::string& service_name,
    const std::string& version_filter,
    const std::string& region_filter,
    const std::string& environment_filter,
    const std::vector<std::string>& required_tags) {
    
    auto all_instances = query_all_services(service_name);
    std::vector<ServiceInstance> matching_instances;

    for (const auto& instance : all_instances) {
        const auto& meta = instance.metadata;
        
        // Check version filter
        if (!version_filter.empty() && meta.version != version_filter) {
            continue;
        }
        
        // Check region filter
        if (!region_filter.empty() && meta.region != region_filter) {
            continue;
        }
        
        // Check environment filter
        if (!environment_filter.empty() && meta.environment != environment_filter) {
            continue;
        }
        
        // Check required tags
        bool has_all_tags = true;
        for (const auto& required_tag : required_tags) {
            if (std::find(meta.tags.begin(), meta.tags.end(), required_tag) == meta.tags.end()) {
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

std::unique_ptr<IServiceDiscovery> make_redis_discovery(const std::string& redis_uri, std::chrono::seconds heartbeat_interval) {
    return std::make_unique<RedisServiceDiscovery>(redis_uri, heartbeat_interval);
}

} // namespace shield::discovery