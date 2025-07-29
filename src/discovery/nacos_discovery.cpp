// shield/src/discovery/nacos_discovery.cpp
#include "shield/discovery/nacos_discovery.hpp"
#include "shield/core/logger.hpp"
#include <iostream>
#include <sstream>
#include <random>
#include <boost/url/url.hpp>
#include <boost/url/parse.hpp>


namespace shield::discovery {

NacosServiceDiscovery::NacosServiceDiscovery(const std::string& nacos_server_address, std::chrono::seconds heartbeat_interval)
    : _heartbeat_interval(heartbeat_interval),
      _running_heartbeat(true) {
    // Parse host and port from nacos_server_address
    boost::urls::url_view u(nacos_server_address);
    _nacos_host = std::string(u.host());
    _nacos_port = std::string(u.port());

    _heartbeat_thread = std::thread(&NacosServiceDiscovery::_heartbeat_loop, this);
}

NacosServiceDiscovery::~NacosServiceDiscovery() {
    _running_heartbeat = false;
    _heartbeat_cv.notify_one(); // Notify heartbeat thread to stop
    if (_heartbeat_thread.joinable()) {
        _heartbeat_thread.join();
    }
}

std::string NacosServiceDiscovery::_send_http_request(http::verb method, const std::string& target, const std::string& body) const {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    auto const results = resolver.resolve(_nacos_host, _nacos_port);
    stream.connect(results);

    http::request<http::string_body> req{method, target, 11 /* HTTP/1.1 */};
    req.set(http::field::host, _nacos_host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    if (!body.empty()) {
        req.set(http::field::content_type, "application/x-www-form-urlencoded");
        req.body() = body;
        req.prepare_payload();
    }

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    if(ec == net::error::eof) {
        // Rationale: Shutdown causes an error::eof when the other end is closed.
        // For this example, we ignore that.
        ec = {};
    }
    if(ec) {
        std::cerr << "HTTP Request Error: " << ec.message() << std::endl;
        return "";
    }

    return res.body();
}

bool NacosServiceDiscovery::register_service(const ServiceInstance& instance, std::optional<std::chrono::seconds> ttl) {
    if (instance.service_name.empty() || instance.instance_id.empty()) {
        return false;
    }

    // Nacos registration parameters
    std::stringstream ss;
    ss << "serviceName=" << instance.service_name
       << "&ip=" << instance.address.substr(instance.address.find("//") + 2, instance.address.find(":", instance.address.find("//") + 2) - (instance.address.find("//") + 2))
       << "&port=" << instance.address.substr(instance.address.find(":", instance.address.find("//") + 2) + 1)
       << "&instanceId=" << instance.instance_id
       << "&weight=1.0"
       << "&enabled=true"
       << "&healthy=true"
       << "&ephemeral=true"; // Ephemeral instances are automatically removed if heartbeat stops

    // Nacos does not directly support TTL for instances in the same way etcd does.
    // Instead, it relies on the client sending heartbeats for ephemeral instances.
    // The 'ttl' parameter here can be used to determine the heartbeat interval for this instance.
    // For now, we'll just use the class's default heartbeat_interval.

    std::string target = "/nacos/v1/ns/instance";
    std::string body = ss.str();

    std::string response = _send_http_request(http::verb::post, target, body);

    if (response.find("ok") != std::string::npos) {
        std::unique_lock<std::mutex> lock(_registered_instances_mutex);
        _registered_instances[instance.instance_id] = instance;
        return true;
    }

    std::cerr << "Nacos register_service failed: " << response << std::endl;
    return false;
}

bool NacosServiceDiscovery::deregister_service(const std::string& service_name, const std::string& instance_id) {
    if (service_name.empty() || instance_id.empty()) {
        return false;
    }

    std::stringstream ss;
    ss << "serviceName=" << service_name
       << "&instanceId=" << instance_id
       << "&ephemeral=true";

    std::string target = "/nacos/v1/ns/instance";
    std::string body = ss.str();

    std::string response = _send_http_request(http::verb::delete_, target, body);

    if (response.find("ok") != std::string::npos) {
        std::unique_lock<std::mutex> lock(_registered_instances_mutex);
        _registered_instances.erase(instance_id);
        return true;
    }

    std::cerr << "Nacos deregister_service failed: " << response << std::endl;
    return false;
}

std::optional<ServiceInstance> NacosServiceDiscovery::query_service(const std::string& service_name) {
    if (service_name.empty()) {
        return std::nullopt;
    }

    std::stringstream ss;
    ss << "serviceName=" << service_name;

    std::string target = "/nacos/v1/ns/instance/list?" + ss.str();
    std::string response = _send_http_request(http::verb::get, target);

    if (response.empty()) {
        return std::nullopt;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(response);
        if (j.contains("hosts") && j["hosts"].is_array()) {
            std::vector<ServiceInstance> available_instances;
            for (const auto& host : j["hosts"]) {
                if (host.contains("ip") && host.contains("port") && host.contains("instanceId")) {
                    ServiceInstance instance;
                    instance.service_name = service_name;
                    instance.instance_id = host.at("instanceId").get<std::string>();
                    instance.address = "tcp://" + host.at("ip").get<std::string>() + ":" + std::to_string(host.at("port").get<int>());
                    // Nacos metadata is usually a map, simplify for now
                    instance.metadata = ServiceMetadata{}; 
                    // Nacos instances are considered active if returned by list API
                    instance.expiration_time = std::chrono::steady_clock::time_point::max(); 
                    available_instances.push_back(instance);
                }
            }

            if (available_instances.empty()) {
                return std::nullopt;
            }

            // Simple random load balancing
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distrib(0, available_instances.size() - 1);
            return available_instances[distrib(gen)];
        }
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "Nacos query_service JSON parse error: " << e.what() << std::endl;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "Nacos query_service JSON exception: " << e.what() << std::endl;
    }

    return std::nullopt;
}

std::vector<ServiceInstance> NacosServiceDiscovery::query_all_services(const std::string& service_name) {
    std::vector<ServiceInstance> instances;
    if (service_name.empty()) {
        return instances;
    }

    std::stringstream ss;
    ss << "serviceName=" << service_name;

    std::string target = "/nacos/v1/ns/instance/list?" + ss.str();
    std::string response = _send_http_request(http::verb::get, target);

    if (response.empty()) {
        return instances;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(response);
        if (j.contains("hosts") && j["hosts"].is_array()) {
            for (const auto& host : j["hosts"]) {
                if (host.contains("ip") && host.contains("port") && host.contains("instanceId")) {
                    ServiceInstance instance;
                    instance.service_name = service_name;
                    instance.instance_id = host.at("instanceId").get<std::string>();
                    instance.address = "tcp://" + host.at("ip").get<std::string>() + ":" + std::to_string(host.at("port").get<int>());
                    instance.metadata = ServiceMetadata{};
                    instance.expiration_time = std::chrono::steady_clock::time_point::max();
                    instances.push_back(instance);
                }
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "Nacos query_all_services JSON parse error: " << e.what() << std::endl;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "Nacos query_all_services JSON exception: " << e.what() << std::endl;
    }

    return instances;
}

std::vector<ServiceInstance> NacosServiceDiscovery::query_services_by_metadata(const std::map<std::string, std::string>& metadata_filters) {
    auto service_name_it = metadata_filters.find("service_name");
    if (service_name_it == metadata_filters.end()) {
        SHIELD_LOG_ERROR << "Nacos query_services_by_metadata requires a 'service_name' in filters.";
        return {};
    }

    auto all_instances = query_all_services(service_name_it->second);
    std::vector<ServiceInstance> matching_instances;

    // Client-side filtering
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

std::vector<ServiceInstance> NacosServiceDiscovery::query_services_by_criteria(
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

void NacosServiceDiscovery::_heartbeat_loop() {
    while (_running_heartbeat) {
        std::unique_lock<std::mutex> lock(_heartbeat_mutex);
        _heartbeat_cv.wait_for(lock, _heartbeat_interval, [this] { return !_running_heartbeat; });

        if (!_running_heartbeat) {
            break;
        }

        // Send heartbeats for all instances registered by this client
        std::unique_lock<std::mutex> instances_lock(_registered_instances_mutex);
        for (const auto& pair : _registered_instances) {
            const ServiceInstance& instance = pair.second;
            std::stringstream ss;
            ss << "serviceName=" << instance.service_name
               << "&instanceId=" << instance.instance_id
               << "&ephemeral=true";

            std::string target = "/nacos/v1/ns/instance/beat";
            std::string body = ss.str();

            // Nacos beat API expects a PUT request with JSON body for instance details
            // However, the simple beat API is a GET/PUT with query params.
            // Let's use the simple beat API for now.
            // The Nacos documentation is a bit confusing here, but the common client behavior
            // is to send a PUT request to /nacos/v1/ns/instance/beat with instanceId and serviceName.
            // Some clients also send a JSON body with more details.
            // For simplicity, we'll stick to query parameters.

            // Nacos beat API: PUT /nacos/v1/ns/instance/beat
            // Required parameters: serviceName, instanceId
            // Optional: clientBeatInterval, cluster, ip, port, scheduled

            // For ephemeral instances, just sending a beat is enough to renew.
            // The 'beat' API is usually simpler than re-registering.
            std::string beat_response = _send_http_request(http::verb::put, target, body);
            if (beat_response.find("ok") == std::string::npos) {
                std::cerr << "Nacos heartbeat failed for instance " << instance.instance_id << ": " << beat_response << std::endl;
            }
        }
    }
}

std::unique_ptr<IServiceDiscovery> make_nacos_discovery(const std::string& nacos_server_address, std::chrono::seconds heartbeat_interval) {
    return std::make_unique<NacosServiceDiscovery>(nacos_server_address, heartbeat_interval);
}

} // namespace shield::discovery
