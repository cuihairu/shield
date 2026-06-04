// shield/src/annotations/component_registry.cpp
#include "shield/annotations/component_registry.hpp"

#include <stdexcept>

#include "shield/log/logger.hpp"

namespace shield::annotations {

// =====================================
// ComponentRegistry 实现
// =====================================

void ComponentRegistry::auto_configure(core::ApplicationContext& context) {
    SHIELD_LOG_INFO << "[ComponentRegistry] Auto-configuring ApplicationContext with "
                    << component_factories_.size() << " components, "
                    << service_factories_.size() << " services, "
                    << configuration_factories_.size() << " configurations";

    // 注册所有组件
    for (const auto& [type_id, registrar] : component_app_context_registrars_) {
        auto it = component_metadata_.find(type_id);
        if (it != component_metadata_.end()) {
            const auto& metadata = it->second;
            std::string name =
                metadata.value.empty()
                    ? (metadata.name.empty() ? type_id.name() : metadata.name)
                    : metadata.value;

            SHIELD_LOG_INFO << "[ComponentRegistry] Registering component: " << name
                            << " (primary: " << (metadata.primary ? "yes" : "no")
                            << ")";
            try {
                registrar(context);
            } catch (const std::exception& e) {
                SHIELD_LOG_ERROR << "[ComponentRegistry] Failed to register component: "
                                 << name << " error: " << e.what();
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

            SHIELD_LOG_INFO << "[ComponentRegistry] Registering service: " << name;
            try {
                registrar(context);
            } catch (const std::exception& e) {
                SHIELD_LOG_ERROR << "[ComponentRegistry] Failed to register service: "
                                 << name << " error: " << e.what();
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

            SHIELD_LOG_INFO << "[ComponentRegistry] Registering configuration: "
                            << name << " (proxy_bean_methods: "
                            << (metadata.proxy_bean_methods ? "yes" : "no") << ")";
            try {
                registrar(context);
            } catch (const std::exception& e) {
                SHIELD_LOG_ERROR
                    << "[ComponentRegistry] Failed to register configuration: "
                    << name << " error: " << e.what();
            }
        }
    }

    SHIELD_LOG_INFO << "[ComponentRegistry] Auto-configuration complete";
}

void ComponentRegistry::auto_configure(di::AdvancedContainer& container) {
    SHIELD_LOG_INFO << "[ComponentRegistry] Auto-configuring AdvancedContainer with "
                    << component_factories_.size() << " components, "
                    << service_factories_.size() << " services";

    for (const auto& [type_id, registrar] : component_container_registrars_) {
        auto it = component_metadata_.find(type_id);
        if (it != component_metadata_.end()) {
            const auto& metadata = it->second;
            SHIELD_LOG_INFO << "[ComponentRegistry] Registering component: "
                            << type_id.name()
                            << " (primary: " << (metadata.primary ? "yes" : "no")
                            << ")";
            registrar(container);
        }
    }

    for (const auto& [type_id, registrar] : service_container_registrars_) {
        SHIELD_LOG_INFO << "[ComponentRegistry] Registering service: "
                        << type_id.name();
        registrar(container);
    }

    for (const auto& [type_id, registrar] : configuration_container_registrars_) {
        SHIELD_LOG_INFO << "[ComponentRegistry] Registering configuration: "
                        << type_id.name();
        registrar(container);
    }

    SHIELD_LOG_INFO
        << "[ComponentRegistry] AdvancedContainer auto-configuration complete";
}

}  // namespace shield::annotations
