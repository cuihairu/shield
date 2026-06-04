// [CORE]
#pragma once

#include <any>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "shield/core/service.hpp"

// Forward declarations
namespace shield::config {
class Configuration;
}

namespace shield::core {
class StarterManager;

class ApplicationContext {
public:
    static ApplicationContext& instance();

    void init_all();
    void start_all();
    void stop_all();

    void configure_with(
        std::unique_ptr<shield::config::Configuration> configuration);

    void configure_with_starters(
        std::unique_ptr<StarterManager> starter_manager);

    template <typename T, typename... Args>
    std::shared_ptr<T> register_service(Args&&... args) {
        static_assert(std::is_base_of_v<Service, T>,
                      "T must inherit from Service");
        auto service = std::make_shared<T>(std::forward<Args>(args)...);
        m_services_by_order.push_back(service);
        m_beans_by_name[service->name()] = service;
        m_bean_type_to_name[std::type_index(typeid(T))] = service->name();
        return service;
    }

    template <typename T>
    void register_service(const std::string& name, std::shared_ptr<T> service) {
        static_assert(std::is_base_of_v<Service, T>,
                      "T must inherit from Service");
        m_services_by_order.push_back(service);
        m_beans_by_name[name] = service;
        m_bean_type_to_name[std::type_index(typeid(T))] = name;
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
    void register_bean(const std::string& name, std::shared_ptr<T> bean) {
        if (!bean) {
            throw std::invalid_argument("Cannot register null bean: " + name);
        }
        if (m_beans_by_name.count(name)) {
            throw std::runtime_error("Bean with name '" + name +
                                     "' already exists.");
        }
        m_beans_by_name[name] = std::move(bean);
        m_bean_type_to_name[std::type_index(typeid(T))] = name;
    }

    template <typename T>
    std::shared_ptr<T> get_bean(const std::string& name) const {
        auto it = m_beans_by_name.find(name);
        if (it == m_beans_by_name.end()) {
            throw std::runtime_error("Bean with name '" + name +
                                     "' not found.");
        }
        try {
            return std::any_cast<std::shared_ptr<T>>(it->second);
        } catch (const std::bad_any_cast&) {
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

private:
    ApplicationContext() = default;

    std::vector<std::shared_ptr<Service>> m_services_by_order;
    std::unordered_map<std::string, std::any> m_beans_by_name;
    std::unordered_map<std::type_index, std::string> m_bean_type_to_name;
};

}  // namespace shield::core
