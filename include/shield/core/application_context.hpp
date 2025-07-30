#pragma once

#include <boost/any.hpp>
#include <boost/type_index.hpp>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/core/service.hpp"

// Forward declarations
namespace shield::config {
class Configuration;
}

namespace shield::core {

class ApplicationContext {
public:
    static ApplicationContext& instance();

    void init_all();
    void start_all();
    void stop_all();

    // Configure the application context using Configuration classes
    void configure_with(
        std::unique_ptr<shield::config::Configuration> configuration);

private:
    ApplicationContext() = default;

    void subscribe_to_config_reloads(const std::shared_ptr<Service>& service);

    // Service registry (ordered for lifecycle management)
    std::vector<std::shared_ptr<Service>> m_services_by_order;

    // Generic bean registry (name -> boost::any<shared_ptr<T>>)
    std::unordered_map<std::string, boost::any> m_beans_by_name;
    std::unordered_map<std::type_index, std::string> m_bean_type_to_name;

    // Template implementations (moved to .hpp for simplicity)
public:
    template <typename T, typename... Args>
    std::shared_ptr<T> register_service(Args&&... args) {
        static_assert(std::is_base_of_v<Service, T>,
                      "T must inherit from Service");
        auto service = std::make_shared<T>(std::forward<Args>(args)...);
        m_services_by_order.push_back(service);
        m_beans_by_name[service->name()] =
            service;  // Store service as a named bean
        m_bean_type_to_name[std::type_index(typeid(T))] = service->name();

        subscribe_to_config_reloads(service);

        return service;
    }

    // Register an already created service instance
    template <typename T>
    void register_service(const std::string& name, std::shared_ptr<T> service) {
        static_assert(std::is_base_of_v<Service, T>,
                      "T must inherit from Service");
        m_services_by_order.push_back(service);
        m_beans_by_name[name] = service;
        m_bean_type_to_name[std::type_index(typeid(T))] = name;

        subscribe_to_config_reloads(service);
    }

    template <typename T>
    std::shared_ptr<T> get_service() const {
        auto it = m_bean_type_to_name.find(std::type_index(typeid(T)));
        if (it != m_bean_type_to_name.end()) {
            return get_bean<T>(it->second);
        }
        return nullptr;
    }

    template <typename T, typename... Args>
    std::shared_ptr<T> register_bean(const std::string& name, Args&&... args) {
        if (m_beans_by_name.count(name)) {
            throw std::runtime_error("Bean with name '" + name +
                                     "' already exists.");
        }
        auto bean = std::make_shared<T>(std::forward<Args>(args)...);
        m_beans_by_name[name] = bean;
        m_bean_type_to_name[std::type_index(typeid(T))] = name;
        return bean;
    }

    template <typename T>
    std::shared_ptr<T> get_bean(const std::string& name) const {
        auto it = m_beans_by_name.find(name);
        if (it == m_beans_by_name.end()) {
            throw std::runtime_error("Bean with name '" + name +
                                     "' not found.");
        }
        try {
            return boost::any_cast<std::shared_ptr<T>>(it->second);
        } catch (const boost::bad_any_cast& e) {
            throw std::runtime_error("Failed to cast bean '" + name +
                                     "' to requested type.");
        }
    }

    template <typename T>
    std::shared_ptr<T> get_bean() const {
        auto it = m_bean_type_to_name.find(std::type_index(typeid(T)));
        if (it != m_bean_type_to_name.end()) {
            return get_bean<T>(it->second);
        }
        return nullptr;
    }
};

}  // namespace shield::core