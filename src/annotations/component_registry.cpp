// shield/src/annotations/component_registry.cpp
#include "shield/annotations/component_registry.hpp"

#include <iostream>
#include <stdexcept>

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
    for (const auto& [type_id, registrar] : component_app_context_registrars_) {
        auto it = component_metadata_.find(type_id);
        if (it != component_metadata_.end()) {
            const auto& metadata = it->second;
            std::string name =
                metadata.value.empty()
                    ? (metadata.name.empty() ? type_id.name() : metadata.name)
                    : metadata.value;

            std::cout << "[ComponentRegistry] Registering component: " << name
                      << " (primary: " << (metadata.primary ? "yes" : "no")
                      << ")" << std::endl;
            try {
                registrar(context);
            } catch (const std::exception& e) {
                std::cout << "[ComponentRegistry] Failed to register component: "
                          << name << " error: " << e.what() << std::endl;
            }
        }
    }

    // 注册所有服务
    for (const auto& [type_id, registrar] : service_app_context_registrars_) {
        auto it = service_metadata_.find(type_id);
        if (it != service_metadata_.end()) {
            const auto& metadata = it->second;
            std::string name =
                metadata.value.empty() ? type_id.name() : metadata.value;

            std::cout << "[ComponentRegistry] Registering service: " << name
                      << std::endl;
            try {
                registrar(context);
            } catch (const std::exception& e) {
                std::cout << "[ComponentRegistry] Failed to register service: "
                          << name << " error: " << e.what() << std::endl;
            }
        }
    }

    // 注册所有配置类
    for (const auto& [type_id, registrar] :
         configuration_app_context_registrars_) {
        auto it = configuration_metadata_.find(type_id);
        if (it != configuration_metadata_.end()) {
            const auto& metadata = it->second;
            std::string name =
                metadata.name.empty() ? type_id.name() : metadata.name;

            std::cout << "[ComponentRegistry] Registering configuration: "
                      << name << " (proxy_bean_methods: "
                      << (metadata.proxy_bean_methods ? "yes" : "no") << ")"
                      << std::endl;
            try {
                registrar(context);
            } catch (const std::exception& e) {
                std::cout
                    << "[ComponentRegistry] Failed to register configuration: "
                    << name << " error: " << e.what() << std::endl;
            }
        }
    }

    std::cout << "[ComponentRegistry] Auto-configuration complete" << std::endl;
}

void ComponentRegistry::auto_configure(di::AdvancedContainer& container) {
    std::cout << "[ComponentRegistry] Auto-configuring AdvancedContainer with "
              << component_factories_.size() << " components, "
              << service_factories_.size() << " services" << std::endl;

    for (const auto& [type_id, registrar] : component_container_registrars_) {
        auto it = component_metadata_.find(type_id);
        if (it != component_metadata_.end()) {
            const auto& metadata = it->second;
            std::cout << "[ComponentRegistry] Registering component: "
                      << type_id.name()
                      << " (primary: " << (metadata.primary ? "yes" : "no")
                      << ")" << std::endl;
            registrar(container);
        }
    }

    for (const auto& [type_id, registrar] : service_container_registrars_) {
        std::cout << "[ComponentRegistry] Registering service: "
                  << type_id.name() << std::endl;
        registrar(container);
    }

    for (const auto& [type_id, registrar] : configuration_container_registrars_) {
        std::cout << "[ComponentRegistry] Registering configuration: "
                  << type_id.name() << std::endl;
        registrar(container);
    }

    std::cout
        << "[ComponentRegistry] AdvancedContainer auto-configuration complete"
        << std::endl;
}

}  // namespace shield::annotations
