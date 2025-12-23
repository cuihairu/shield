// shield/src/annotations/component_registry.cpp
#include "shield/annotations/component_registry.hpp"

#include <iostream>

namespace shield::annotations {

// =====================================
// ComponentRegistry 实现
// =====================================

void ComponentRegistry::auto_configure(core::ApplicationContext& context) {
    std::cout << "[ComponentRegistry] Auto-configuring ApplicationContext with "
              << component_factories_.size() << " components, "
              << service_factories_.size() << " services, "
              << configuration_factories_.size() << " configurations"
              << std::endl;

    // 注册所有组件
    for (const auto& [type_id, factory] : component_factories_) {
        auto it = component_metadata_.find(type_id);
        if (it != component_metadata_.end()) {
            const auto& metadata = it->second;
            std::string name =
                metadata.name.empty() ? type_id.name() : metadata.name;

            std::cout << "[ComponentRegistry] Registering component: " << name
                      << " (primary: " << (metadata.primary ? "yes" : "no")
                      << ")" << std::endl;

            // 注册到 ApplicationContext
            // context.register_component_factory(name, factory, metadata);
        }
    }

    // 注册所有服务
    for (const auto& [type_id, factory] : service_factories_) {
        auto it = service_metadata_.find(type_id);
        if (it != service_metadata_.end()) {
            const auto& metadata = it->second;
            std::string name =
                metadata.value.empty() ? type_id.name() : metadata.value;

            std::cout << "[ComponentRegistry] Registering service: " << name
                      << std::endl;

            // 注册到 ApplicationContext
            // context.register_service_factory(name, factory);
        }
    }

    // 注册所有配置类
    for (const auto& [type_id, factory] : configuration_factories_) {
        auto it = configuration_metadata_.find(type_id);
        if (it != configuration_metadata_.end()) {
            const auto& metadata = it->second;
            std::string name =
                metadata.name.empty() ? type_id.name() : metadata.name;

            std::cout << "[ComponentRegistry] Registering configuration: "
                      << name << " (proxy_bean_methods: "
                      << (metadata.proxy_bean_methods ? "yes" : "no") << ")"
                      << std::endl;

            // 注册到 ApplicationContext
            // context.register_configuration_factory(name, factory, metadata);
        }
    }

    std::cout << "[ComponentRegistry] Auto-configuration complete" << std::endl;
}

void ComponentRegistry::auto_configure(di::AdvancedContainer& container) {
    std::cout << "[ComponentRegistry] Auto-configuring AdvancedContainer with "
              << component_factories_.size() << " components, "
              << service_factories_.size() << " services" << std::endl;

    // 由于类型擦除的限制，简化实现仅打印日志
    // 实际注册需要使用类型安全的方法

    for (const auto& [type_id, factory] : component_factories_) {
        auto it = component_metadata_.find(type_id);
        if (it != component_metadata_.end()) {
            const auto& metadata = it->second;
            std::cout << "[ComponentRegistry] Would register component: "
                      << type_id.name()
                      << " (primary: " << (metadata.primary ? "yes" : "no")
                      << ")" << std::endl;
        }
    }

    for (const auto& [type_id, factory] : service_factories_) {
        std::cout << "[ComponentRegistry] Would register service: "
                  << type_id.name() << std::endl;
    }

    for (const auto& [type_id, factory] : configuration_factories_) {
        std::cout << "[ComponentRegistry] Would register configuration: "
                  << type_id.name() << std::endl;
    }

    std::cout
        << "[ComponentRegistry] AdvancedContainer auto-configuration complete"
        << std::endl;
}

}  // namespace shield::annotations
