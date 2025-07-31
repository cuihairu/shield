#pragma once

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "shield/core/application_context.hpp"
#include "shield/di/advanced_container.hpp"

namespace shield::annotations {

/**
 * @brief Component annotation metadata
 */
struct ComponentMetadata {
    std::string name;
    std::string value;  // Component value/name
    bool primary = false;
    std::vector<std::string> profiles;

    ComponentMetadata(const std::string& comp_name = "",
                      const std::string& comp_value = "",
                      bool is_primary = false)
        : name(comp_name), value(comp_value), primary(is_primary) {}
};

/**
 * @brief Service annotation metadata
 */
struct ServiceMetadata {
    std::string name;
    std::string value;

    ServiceMetadata(const std::string& svc_name = "",
                    const std::string& svc_value = "")
        : name(svc_name), value(svc_value) {}
};

/**
 * @brief Configuration annotation metadata
 */
struct ConfigurationMetadata {
    std::string name;
    bool proxy_bean_methods = true;

    ConfigurationMetadata(const std::string& config_name = "",
                          bool proxy = true)
        : name(config_name), proxy_bean_methods(proxy) {}
};

/**
 * @brief Bean annotation metadata
 */
struct BeanMetadata {
    std::string name;
    std::vector<std::string> init_methods;
    std::vector<std::string> destroy_methods;

    BeanMetadata(const std::string& bean_name = "") : name(bean_name) {}
};

/**
 * @brief Component registry for annotation-driven development
 */
class ComponentRegistry {
public:
    static ComponentRegistry& instance() {
        static ComponentRegistry registry;
        return registry;
    }

    /**
     * @brief Register a component with metadata
     */
    template <typename T>
    static void register_component(const ComponentMetadata& metadata = {}) {
        auto& registry = instance();
        std::type_index type_id(typeid(T));

        registry.component_metadata_[type_id] = metadata;
        registry.component_factories_[type_id] = []() -> std::shared_ptr<void> {
            return std::static_pointer_cast<void>(std::make_shared<T>());
        };
    }

    /**
     * @brief Register a service with metadata
     */
    template <typename T>
    static void register_service(const ServiceMetadata& metadata = {}) {
        auto& registry = instance();
        std::type_index type_id(typeid(T));

        registry.service_metadata_[type_id] = metadata;
        registry.service_factories_[type_id] = []() -> std::shared_ptr<void> {
            return std::static_pointer_cast<void>(std::make_shared<T>());
        };
    }

    /**
     * @brief Register a configuration class with metadata
     */
    template <typename T>
    static void register_configuration(
        const ConfigurationMetadata& metadata = {}) {
        auto& registry = instance();
        std::type_index type_id(typeid(T));

        registry.configuration_metadata_[type_id] = metadata;
        registry.configuration_factories_[type_id] =
            []() -> std::shared_ptr<void> {
            return std::static_pointer_cast<void>(std::make_shared<T>());
        };
    }

    /**
     * @brief Auto-register all annotated components with ApplicationContext
     */
    void auto_configure(core::ApplicationContext& context);

    /**
     * @brief Auto-register all annotated components with AdvancedContainer
     */
    void auto_configure(di::AdvancedContainer& container);

    /**
     * @brief Get component metadata
     */
    template <typename T>
    std::optional<ComponentMetadata> get_component_metadata() const {
        std::type_index type_id(typeid(T));
        auto it = component_metadata_.find(type_id);
        if (it != component_metadata_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /**
     * @brief Check if type is registered as component
     */
    template <typename T>
    bool is_component() const {
        return component_metadata_.count(std::type_index(typeid(T))) > 0;
    }

    /**
     * @brief Check if type is registered as service
     */
    template <typename T>
    bool is_service() const {
        return service_metadata_.count(std::type_index(typeid(T))) > 0;
    }

    /**
     * @brief Check if type is registered as configuration
     */
    template <typename T>
    bool is_configuration() const {
        return configuration_metadata_.count(std::type_index(typeid(T))) > 0;
    }

private:
    ComponentRegistry() = default;

    std::unordered_map<std::type_index, ComponentMetadata> component_metadata_;
    std::unordered_map<std::type_index, ServiceMetadata> service_metadata_;
    std::unordered_map<std::type_index, ConfigurationMetadata>
        configuration_metadata_;

    std::unordered_map<std::type_index, std::function<std::shared_ptr<void>()>>
        component_factories_;
    std::unordered_map<std::type_index, std::function<std::shared_ptr<void>()>>
        service_factories_;
    std::unordered_map<std::type_index, std::function<std::shared_ptr<void>()>>
        configuration_factories_;
};

}  // namespace shield::annotations

/**
 * @brief Annotation macros (Spring Boot style)
 */

// Component annotation
#define SHIELD_COMPONENT(name_value)                                          \
    static inline auto _shield_component_reg_##__COUNTER__ = []() {           \
        shield::annotations::ComponentRegistry::register_component<           \
            std::remove_cv_t<std::remove_reference_t<decltype(*this)>>>(      \
            shield::annotations::ComponentMetadata(#name_value, name_value)); \
        return 0;                                                             \
    }();

// Service annotation
#define SHIELD_SERVICE(name_value)                                          \
    static inline auto _shield_service_reg_##__COUNTER__ = []() {           \
        shield::annotations::ComponentRegistry::register_service<           \
            std::remove_cv_t<std::remove_reference_t<decltype(*this)>>>(    \
            shield::annotations::ServiceMetadata(#name_value, name_value)); \
        return 0;                                                           \
    }();

// Configuration annotation
#define SHIELD_CONFIGURATION(name_value)                                 \
    static inline auto _shield_config_reg_##__COUNTER__ = []() {         \
        shield::annotations::ComponentRegistry::register_configuration<  \
            std::remove_cv_t<std::remove_reference_t<decltype(*this)>>>( \
            shield::annotations::ConfigurationMetadata(#name_value));    \
        return 0;                                                        \
    }();

// Autowired annotation (for member variables)
#define SHIELD_AUTOWIRED \
    /* This would be used with additional tooling/code generation */

// Bean annotation (for methods in @Configuration classes)
#define SHIELD_BEAN(name_value) \
    /* This would require method-level metadata registration */

// Component scan annotation
#define SHIELD_COMPONENT_SCAN(...) \
    /* This would trigger automatic scanning of specified packages */

/**
 * @brief Utility class for annotation-driven component scanning
 */
namespace shield::annotations {

class ComponentScanner {
public:
    /**
     * @brief Scan and auto-register components in specified namespaces
     */
    static void scan_components(
        const std::vector<std::string>& base_packages = {});

    /**
     * @brief Scan and register all globally registered components
     */
    static void scan_all_components();

    /**
     * @brief Register scanned components with ApplicationContext
     */
    static void configure_application_context(
        core::ApplicationContext& context);

    /**
     * @brief Register scanned components with DI Container
     */
    static void configure_di_container(di::AdvancedContainer& container);
};

/**
 * @brief Conditional component registration
 */
class ConditionalRegistry {
public:
    /**
     * @brief Register component conditionally based on property
     */
    template <typename T>
    static void register_on_property(
        const std::string& property,
        const std::string& expected_value = "true") {
        // Implementation would check configuration properties
        auto& config = shield::config::ConfigManager::instance();
        // This would need property tree navigation
        // if (config.has_property(property) && config.get_property(property) ==
        // expected_value) {
        //     ComponentRegistry::register_component<T>();
        // }
    }

    /**
     * @brief Register component conditionally based on missing bean
     */
    template <typename T, typename MissingBeanType>
    static void register_on_missing_bean() {
        // Implementation would check if MissingBeanType is already registered
        if (!ComponentRegistry::instance().is_component<MissingBeanType>()) {
            ComponentRegistry::register_component<T>();
        }
    }

    /**
     * @brief Register component conditionally based on class presence
     */
    template <typename T>
    static void register_on_class(const std::string& class_name) {
        // Implementation would check if specified class exists in classpath
        // This is more challenging in C++ without reflection
        ComponentRegistry::register_component<T>();
    }
};

}  // namespace shield::annotations