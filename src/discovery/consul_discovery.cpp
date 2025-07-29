// shield/src/discovery/consul_discovery.cpp
#include "shield/discovery/consul_discovery.hpp"

#include <algorithm>
#include <boost/url/parse.hpp>
#include <boost/url/url.hpp>
#include <iostream>
#include <map>
#include <random>
#include <sstream>

namespace shield::discovery {

ConsulServiceDiscovery::ConsulServiceDiscovery(
    const std::string& consul_server_address,
    std::chrono::seconds check_interval)
    : _check_interval(check_interval), _running_check(true) {
    // Parse host and port from consul_server_address
    boost::urls::url_view u(consul_server_address);
    _consul_host = std::string(u.host());
    _consul_port = std::string(u.port());

    _check_thread = std::thread(&ConsulServiceDiscovery::_check_loop, this);
}

ConsulServiceDiscovery::~ConsulServiceDiscovery() {
    _running_check = false;
    _check_cv.notify_one();  // Notify check thread to stop
    if (_check_thread.joinable()) {
        _check_thread.join();
    }
}

std::string ConsulServiceDiscovery::_send_http_request(
    http::verb method, const std::string& target, const std::string& body,
    const std::string& content_type) const {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    auto const results = resolver.resolve(_consul_host, _consul_port);
    stream.connect(results);

    http::request<http::string_body> req{method, target, 11 /* HTTP/1.1 */};
    req.set(http::field::host, _consul_host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::content_type, content_type);
    if (!body.empty()) {
        req.body() = body;
        req.prepare_payload();
    }

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    if (ec == net::error::eof) {
        ec = {};
    }
    if (ec) {
        std::cerr << "HTTP Request Error: " << ec.message() << std::endl;
        return "";
    }

    return res.body();
}

bool ConsulServiceDiscovery::register_service(
    const ServiceInstance& instance, std::optional<std::chrono::seconds> ttl) {
    if (instance.service_name.empty() || instance.instance_id.empty()) {
        return false;
    }

    nlohmann::json service_json;
    service_json["ID"] = instance.instance_id;
    service_json["Name"] = instance.service_name;
    // Assuming address is in format tcp://ip:port
    std::string ip = instance.address.substr(
        instance.address.find("//") + 2,
        instance.address.find(":", instance.address.find("//") + 2) -
            (instance.address.find("//") + 2));
    int port = std::stoi(instance.address.substr(
        instance.address.find(":", instance.address.find("//") + 2) + 1));
    service_json["Address"] = ip;
    service_json["Port"] = port;
    // Convert metadata to Consul tags
    nlohmann::json meta_json =
        nlohmann::json::parse(instance.metadata.to_json());
    service_json["Meta"] = meta_json;
    std::vector<std::string> tags;
    if (meta_json.is_object()) {
        for (auto& [key, val] : meta_json.items()) {
            if (val.is_string()) {
                tags.push_back(key + ":" + val.get<std::string>());
            }
        }
    }
    service_json["Tags"] = tags;

    // Add health check
    nlohmann::json check_json;
    std::string check_id = "service:" + instance.instance_id;
    check_json["DeregisterCriticalServiceAfter"] =
        "1m";  // Deregister after 1 minute if check fails
    check_json["TTL"] = std::to_string(ttl ? ttl->count() : 10) +
                        "s";  // Default TTL to 10s if not provided
    check_json["CheckID"] = check_id;
    check_json["Name"] = "service:" + instance.service_name + ":" +
                         instance.instance_id + ":healthcheck";
    service_json["Check"] = check_json;

    std::string target = "/v1/agent/service/register";
    std::string body = service_json.dump();

    std::string response = _send_http_request(http::verb::put, target, body);

    // Consul API returns empty body on success for register
    if (response.empty()) {
        std::unique_lock<std::mutex> lock(_registered_checks_mutex);
        _registered_checks[instance.instance_id] = check_id;
        return true;
    }

    std::cerr << "Consul register_service failed: " << response << std::endl;
    return false;
}

bool ConsulServiceDiscovery::deregister_service(
    const std::string& service_name, const std::string& instance_id) {
    if (service_name.empty() || instance_id.empty()) {
        return false;
    }

    // Consul deregister by service ID
    std::string target = "/v1/agent/service/deregister/" + instance_id;
    std::string response = _send_http_request(http::verb::put, target);

    if (response.empty()) {
        std::unique_lock<std::mutex> lock(_registered_checks_mutex);
        _registered_checks.erase(instance_id);
        return true;
    }

    std::cerr << "Consul deregister_service failed: " << response << std::endl;
    return false;
}

std::optional<ServiceInstance> ConsulServiceDiscovery::query_service(
    const std::string& service_name) {
    if (service_name.empty()) {
        return std::nullopt;
    }

    std::string target = "/v1/catalog/service/" + service_name;
    std::string response = _send_http_request(http::verb::get, target);

    if (response.empty()) {
        return std::nullopt;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(response);
        if (j.is_array()) {
            std::vector<ServiceInstance> available_instances;
            for (const auto& entry : j) {
                if (entry.contains("ServiceAddress") &&
                    entry.contains("ServicePort") &&
                    entry.contains("ServiceID")) {
                    ServiceInstance instance;
                    instance.service_name = service_name;
                    instance.instance_id =
                        entry.at("ServiceID").get<std::string>();
                    instance.address =
                        "tcp://" +
                        entry.at("ServiceAddress").get<std::string>() + ":" +
                        std::to_string(entry.at("ServicePort").get<int>());
                    instance.metadata =
                        entry.contains("ServiceMeta")
                            ? ServiceMetadata::from_json(
                                  entry.at("ServiceMeta").dump())
                            : ServiceMetadata{};
                    instance.expiration_time = std::chrono::steady_clock::
                        time_point::max();  // Consul manages health, not
                                            // explicit TTL here
                    available_instances.push_back(instance);
                }
            }

            if (available_instances.empty()) {
                return std::nullopt;
            }

            // Simple random load balancing
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> distrib(
                0, available_instances.size() - 1);
            return available_instances[distrib(gen)];
        }
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "Consul query_service JSON parse error: " << e.what()
                  << std::endl;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "Consul query_service JSON exception: " << e.what()
                  << std::endl;
    }

    return std::nullopt;
}

std::vector<ServiceInstance> ConsulServiceDiscovery::query_all_services(
    const std::string& service_name) {
    std::vector<ServiceInstance> instances;
    if (service_name.empty()) {
        return instances;
    }

    std::string target = "/v1/catalog/service/" + service_name;
    std::string response = _send_http_request(http::verb::get, target);

    if (response.empty()) {
        return instances;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(response);
        if (j.is_array()) {
            for (const auto& entry : j) {
                if (entry.contains("ServiceAddress") &&
                    entry.contains("ServicePort") &&
                    entry.contains("ServiceID")) {
                    ServiceInstance instance;
                    instance.service_name = service_name;
                    instance.instance_id =
                        entry.at("ServiceID").get<std::string>();
                    instance.address =
                        "tcp://" +
                        entry.at("ServiceAddress").get<std::string>() + ":" +
                        std::to_string(entry.at("ServicePort").get<int>());
                    instance.metadata =
                        entry.contains("ServiceMeta")
                            ? ServiceMetadata::from_json(
                                  entry.at("ServiceMeta").dump())
                            : ServiceMetadata{};
                    instance.expiration_time =
                        std::chrono::steady_clock::time_point::max();
                    instances.push_back(instance);
                }
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "Consul query_all_services JSON parse error: " << e.what()
                  << std::endl;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "Consul query_all_services JSON exception: " << e.what()
                  << std::endl;
    }

    return instances;
}

std::vector<ServiceInstance> ConsulServiceDiscovery::query_services_by_metadata(
    const std::map<std::string, std::string>& metadata_filters) {
    std::vector<ServiceInstance> matching_instances;
    if (metadata_filters.empty()) {
        return matching_instances;
    }

    // For Consul, we can only filter by tags. We assume the service name is one
    // of the filters.
    auto service_name_it = metadata_filters.find("service_name");
    if (service_name_it == metadata_filters.end()) {
        // Or, we could query all services and filter client-side, but that's
        // inefficient. For now, we require a service_name for a targeted query.
        std::cerr << "Consul query_services_by_metadata requires a "
                     "'service_name' in filters."
                  << std::endl;
        return matching_instances;
    }
    std::string service_name = service_name_it->second;

    std::string target = "/v1/catalog/service/" + service_name;
    std::vector<std::string> tags_to_filter;
    for (const auto& filter : metadata_filters) {
        if (filter.first != "service_name") {
            tags_to_filter.push_back(filter.first + ":" + filter.second);
        }
    }

    if (!tags_to_filter.empty()) {
        target += "?";
        for (size_t i = 0; i < tags_to_filter.size(); ++i) {
            target += "tag=" + tags_to_filter[i];
            if (i < tags_to_filter.size() - 1) {
                target += "&";
            }
        }
    }

    std::string response = _send_http_request(http::verb::get, target);
    if (response.empty()) {
        return matching_instances;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(response);
        if (j.is_array()) {
            for (const auto& entry : j) {
                if (entry.contains("ServiceAddress") &&
                    entry.contains("ServicePort") &&
                    entry.contains("ServiceID")) {
                    ServiceInstance instance;
                    instance.service_name = service_name;
                    instance.instance_id =
                        entry.at("ServiceID").get<std::string>();
                    instance.address =
                        "tcp://" +
                        entry.at("ServiceAddress").get<std::string>() + ":" +
                        std::to_string(entry.at("ServicePort").get<int>());
                    instance.metadata =
                        entry.contains("ServiceMeta")
                            ? ServiceMetadata::from_json(
                                  entry.at("ServiceMeta").dump())
                            : ServiceMetadata{};
                    instance.expiration_time =
                        std::chrono::steady_clock::time_point::max();
                    matching_instances.push_back(instance);
                }
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "Consul query_services_by_metadata JSON parse error: "
                  << e.what() << std::endl;
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "Consul query_services_by_metadata JSON exception: "
                  << e.what() << std::endl;
    }

    return matching_instances;
}

std::vector<ServiceInstance> ConsulServiceDiscovery::query_services_by_criteria(
    const std::string& service_name, const std::string& version_filter,
    const std::string& region_filter, const std::string& environment_filter,
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
        if (!environment_filter.empty() &&
            meta.environment != environment_filter) {
            continue;
        }

        // Check required tags
        bool has_all_tags = true;
        for (const auto& required_tag : required_tags) {
            if (std::find(meta.tags.begin(), meta.tags.end(), required_tag) ==
                meta.tags.end()) {
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

void ConsulServiceDiscovery::_check_loop() {
    while (_running_check) {
        std::unique_lock<std::mutex> lock(_check_mutex);
        _check_cv.wait_for(lock, _check_interval,
                           [this] { return !_running_check; });

        if (!_running_check) {
            break;
        }

        // Send heartbeats for all registered checks
        std::unique_lock<std::mutex> checks_lock(_registered_checks_mutex);
        for (const auto& pair : _registered_checks) {
            const std::string& check_id = pair.second;
            std::string target = "/v1/agent/check/pass/" + check_id;
            // Consul API expects PUT with empty body for check pass
            std::string response = _send_http_request(
                http::verb::put, target, "",
                "text/plain");  // Content-Type can be anything, or omitted
            if (!response.empty()) {
                std::cerr << "Consul heartbeat failed for check " << check_id
                          << ": " << response << std::endl;
            }
        }
    }
}

std::unique_ptr<IServiceDiscovery> make_consul_discovery(
    const std::string& consul_server_address,
    std::chrono::seconds check_interval) {
    return std::make_unique<ConsulServiceDiscovery>(consul_server_address,
                                                    check_interval);
}

}  // namespace shield::discovery