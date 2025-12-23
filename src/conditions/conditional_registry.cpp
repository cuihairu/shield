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
            // 条件满足，注册 bean

            // 由于类型擦除的限制，我们只能打印日志
            // 实际注册需要在编译时知道具体类型
            std::cout << "[ConditionalRegistry] Would register bean: "
                      << (bean_info.name.empty()
                              ? bean_info.bean_type.name()
                              : bean_info.name)
                      << " (condition: " << bean_info.condition->description()
                      << ", lifetime: "
                      << (bean_info.lifetime == di::ServiceLifetime::SINGLETON
                              ? "singleton"
                              : "transient")
                      << ")" << std::endl;

            // TODO: 实际实现需要使用类型擦除或其他机制
            // 当前简化实现仅记录日志

        } else {
            // 条件不满足
            std::cout << "[ConditionalRegistry] Skipped bean: "
                      << (bean_info.name.empty()
                              ? bean_info.bean_type.name()
                              : bean_info.name)
                      << " (condition not met: "
                      << (bean_info.condition ? bean_info.condition->description()
                                            : "none")
                      << ")" << std::endl;
        }
    }
}

void ConditionalBeanRegistry::process_conditional_registrations(
    core::ApplicationContext& context) {
    for (const auto& bean_info : conditional_beans_) {
        if (bean_info.condition && bean_info.condition->matches()) {
            // 注册到 ApplicationContext
            std::cout << "[ConditionalRegistry] Would register to ApplicationContext: "
                      << (bean_info.name.empty()
                              ? bean_info.bean_type.name()
                              : bean_info.name)
                      << " (condition: " << bean_info.condition->description()
                      << ")" << std::endl;

            // TODO: 需要 ApplicationContext 提供相应的注册接口
        }
    }
}

}  // namespace shield::conditions
