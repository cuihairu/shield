// shield/include/shield/discovery/nacos_discovery.hpp
#pragma once

#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"
#include "shield/discovery/service_discovery.hpp"

namespace shield::discovery {

namespace beast = boost::beast;    // from <boost/beast/core.hpp>
namespace http = beast::http;      // from <boost/beast/http.hpp>
namespace net = boost::asio;       // from <boost/asio/io_context.hpp>
using tcp = boost::asio::ip::tcp;  // from <boost/asio/ip/tcp.hpp>

/// @brief An implementation of IServiceDiscovery using Nacos as a backend.
/// This implementation interacts with Nacos via its HTTP REST API.
class NacosServiceDiscovery : public IServiceDiscovery {
public:
    /// @brief Constructs a NacosServiceDiscovery instance.
    /// @param nacos_server_address The Nacos server address (host:port), e.g.,
    /// "127.0.0.1:8848".
    /// @param heartbeat_interval The interval at which registered services are
    /// heartbeated (renewed).
    NacosServiceDiscovery(
        const std::string &nacos_server_address,
        std::chrono::seconds heartbeat_interval = std::chrono::seconds(5));
    ~NacosServiceDiscovery() override;

    // Non-copyable
    NacosServiceDiscovery(const NacosServiceDiscovery &) = delete;
    NacosServiceDiscovery &operator=(const NacosServiceDiscovery &) = delete;

    bool register_service(
        const ServiceInstance &instance,
        std::optional<std::chrono::seconds> ttl = std::nullopt) override;
    bool deregister_service(const std::string &service_name,
                            const std::string &instance_id) override;
    std::optional<ServiceInstance> query_service(
        const std::string &service_name) override;
    std::vector<ServiceInstance> query_all_services(
        const std::string &service_name) override;
    std::vector<ServiceInstance> query_services_by_metadata(
        const std::map<std::string, std::string> &metadata_filters) override;

    std::vector<ServiceInstance> query_services_by_criteria(
        const std::string &service_name, const std::string &version_filter = "",
        const std::string &region_filter = "",
        const std::string &environment_filter = "",
        const std::vector<std::string> &required_tags = {}) override;

private:
    std::string _nacos_host;
    std::string _nacos_port;
    std::chrono::seconds _heartbeat_interval;

    std::thread _heartbeat_thread;
    std::atomic<bool> _running_heartbeat{false};
    std::mutex _heartbeat_mutex;
    std::condition_variable _heartbeat_cv;

    // Store instances registered by this client for heartbeating
    std::mutex _registered_instances_mutex;
    std::map<std::string, ServiceInstance>
        _registered_instances;  // instance_id -> ServiceInstance

    void _heartbeat_loop();

    // Helper to send HTTP requests
    std::string _send_http_request(http::verb method, const std::string &target,
                                   const std::string &body = "") const;
};

/// @brief Factory function to create an instance of NacosServiceDiscovery.
/// @param nacos_server_address The Nacos server address (host:port), e.g.,
/// "127.0.0.1:8848".
/// @param heartbeat_interval The interval at which registered services are
/// heartbeated (renewed).
std::unique_ptr<IServiceDiscovery> make_nacos_discovery(
    const std::string &nacos_server_address,
    std::chrono::seconds heartbeat_interval = std::chrono::seconds(5));

}  // namespace shield::discovery
