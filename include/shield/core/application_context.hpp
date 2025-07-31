#pragma once

#include <boost/any.hpp>
#include <boost/type_index.hpp>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/core/plugin_manager.hpp"
#include "shield/core/service.hpp"
#include "shield/core/starter_manager.hpp"
#include "shield/di/advanced_container.hpp"
#include "shield/events/event_publisher.hpp"
#include "shield/health/health_check.hpp"

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

    // Configure the application context using Starter system
    void configure_with_starters(
        std::unique_ptr<StarterManager> starter_manager);

    // Configure the application context using plugins
    void configure_with_plugins(const std::string& plugins_directory);
    void configure_with_plugins(std::unique_ptr<PluginManager> plugin_manager);

    // Configure with annotations and conditional registration
    void configure_with_annotations();
    void configure_with_conditional_beans();

    // Get the plugin manager
    PluginManager* get_plugin_manager() const { return plugin_manager_.get(); }

    // Get the DI container
    di::AdvancedContainer& get_di_container() { return di_container_; }

    // Get the event publisher
    events::DefaultEventPublisher& get_event_publisher() {
        return event_publisher_;
    }

    // Get the health registry
    health::HealthCheckRegistry& get_health_registry() {
        return health::HealthCheckRegistry::instance();
    }

    // Publish application lifecycle events
    void publish_application_started_event();
    void publish_application_stopped_event();

private:
    ApplicationContext() = default;

    void subscribe_to_config_reloads(const std::shared_ptr<Service>& service);

    // Service registry (ordered for lifecycle management)
    std::vector<std::shared_ptr<Service>> m_services_by_order;

    // Generic bean registry (name -> boost::any<shared_ptr<T>>)
    std::unordered_map<std::string, boost::any> m_beans_by_name;
    std::unordered_map<std::type_index, std::string> m_bean_type_to_name;

    // Plugin manager for dynamic plugin loading
    std::unique_ptr<PluginManager> plugin_manager_;

    // Advanced DI container
    di::AdvancedContainer di_container_;

    // Event publisher for application events
    events::DefaultEventPublisher event_publisher_;

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