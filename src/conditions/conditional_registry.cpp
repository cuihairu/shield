// shield/src/conditions/conditional_registry.cpp
#include "shield/conditions/conditional_registry.hpp"

#include <iostream>

namespace shield::conditions {

// =====================================
// ConditionalBeanRegistry 实现
// =====================================

void ConditionalBeanRegistry::process_conditional_registrations(
    di::AdvancedContainer& container) {
    for (const auto& bean_info : conditional_beans_) {
        if (bean_info.condition && bean_info.condition->matches()) {
            std::cout << "[ConditionalRegistry] Registering bean: "
                      << (bean_info.name.empty() ? bean_info.bean_type.name()
                                                 : bean_info.name)
                      << " (condition: " << bean_info.condition->description()
                      << ", lifetime: "
                      << (bean_info.lifetime == di::ServiceLifetime::SINGLETON
                              ? "singleton"
                              : "transient")
                      << ")" << std::endl;
            if (bean_info.container_registrar) {
                bean_info.container_registrar(container);
            }
        } else {
            // 条件不满足
            std::cout << "[ConditionalRegistry] Skipped bean: "
                      << (bean_info.name.empty() ? bean_info.bean_type.name()
                                                 : bean_info.name)
                      << " (condition not met: "
                      << (bean_info.condition
                              ? bean_info.condition->description()
                              : "none")
                      << ")" << std::endl;
        }
    }
}

void ConditionalBeanRegistry::process_conditional_registrations(
    core::ApplicationContext& context) {
    for (const auto& bean_info : conditional_beans_) {
        if (bean_info.condition && bean_info.condition->matches()) {
            std::cout << "[ConditionalRegistry] Registering bean in "
                         "ApplicationContext: "
                      << (bean_info.name.empty() ? bean_info.bean_type.name()
                                                 : bean_info.name)
                      << " (condition: " << bean_info.condition->description()
                      << ")" << std::endl;
            if (bean_info.context_registrar) {
                bean_info.context_registrar(context);
            }
        }
    }
}

}  // namespace shield::conditions
