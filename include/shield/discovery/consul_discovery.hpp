// shield/include/shield/discovery/consul_discovery.hpp
#pragma once

#include "shield/discovery/service_discovery.hpp"
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <map>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include "nlohmann/json.hpp"

namespace shield::discovery {

namespace beast = boost::beast;         // from <boost/beast/core.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;           // from <boost/asio/io_context.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

/// @brief An implementation of IServiceDiscovery using Consul as a backend.
/// This implementation interacts with Consul via its HTTP REST API.
class ConsulServiceDiscovery : public IServiceDiscovery {
public:
    /// @brief Constructs a ConsulServiceDiscovery instance.
    /// @param consul_server_address The Consul agent address (host:port), e.g., "127.0.0.1:8500".
    /// @param check_interval The interval at which health checks are sent to Consul.
    ConsulServiceDiscovery(const std::string& consul_server_address, std::chrono::seconds check_interval = std::chrono::seconds(10));
    ~ConsulServiceDiscovery() override;

    // Non-copyable
    ConsulServiceDiscovery(const ConsulServiceDiscovery&) = delete;
    ConsulServiceDiscovery& operator=(const ConsulServiceDiscovery&) = delete;

    bool register_service(const ServiceInstance& instance, std::optional<std::chrono::seconds> ttl = std::nullopt) override;
    bool deregister_service(const std::string& service_name, const std::string& instance_id) override;
    std::optional<ServiceInstance> query_service(const std::string& service_name) override;
    std::vector<ServiceInstance> query_all_services(const std::string& service_name) override;
    std::vector<ServiceInstance> query_services_by_metadata(const std::map<std::string, std::string>& metadata_filters) override;
    std::vector<ServiceInstance> query_services_by_criteria(
        const std::string& service_name,
        const std::string& version_filter = "",
        const std::string& region_filter = "",
        const std::string& environment_filter = "",
        const std::vector<std::string>& required_tags = {}) override;

private:
    std::string _consul_host;
    std::string _consul_port;
    std::chrono::seconds _check_interval;

    std::thread _check_thread;
    std::atomic<bool> _running_check{false};
    std::mutex _check_mutex;
    std::condition_variable _check_cv;

    // Store check_id for instances registered by this client for heartbeating
    std::mutex _registered_checks_mutex;
    std::map<std::string, std::string> _registered_checks; // instance_id -> check_id

    void _check_loop();

    // Helper to send HTTP requests
    std::string _send_http_request(http::verb method, const std::string& target, const std::string& body = "", const std::string& content_type = "application/json") const;
};

/// @brief Factory function to create an instance of ConsulServiceDiscovery.
/// @param consul_server_address The Consul agent address (host:port), e.g., "127.0.0.1:8500".
/// @param check_interval The interval at which health checks are sent to Consul.
std::unique_ptr<IServiceDiscovery> make_consul_discovery(const std::string& consul_server_address, std::chrono::seconds check_interval = std::chrono::seconds(10));

} // namespace shield::discovery
