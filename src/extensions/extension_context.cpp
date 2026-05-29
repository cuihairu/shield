#include "shield/extensions/extension_context.hpp"

#include "shield/annotations/component_registry.hpp"
#include "shield/conditions/conditional_registry.hpp"
#include "shield/core/plugin_host.hpp"
#include "shield/events/event_system.hpp"
#include "shield/log/logger.hpp"

namespace shield::extensions {

ExtensionContext& ExtensionContext::instance() {
    static ExtensionContext instance;
    return instance;
}

void ExtensionContext::configure_with_plugins(
    core::ApplicationContext& ctx, const std::string& plugins_directory) {
    if (!plugin_host_) {
        plugin_host_ = std::make_unique<core::PluginHost>();
    }
    plugin_host_->configure(ctx, plugins_directory);
}

void ExtensionContext::configure_with_plugins(
    core::ApplicationContext& ctx,
    std::unique_ptr<core::PluginManager> plugin_manager) {
    try {
        if (!plugin_host_) {
            plugin_host_ = std::make_unique<core::PluginHost>();
        }
        plugin_host_->configure(ctx, std::move(plugin_manager));
    } catch (const std::exception& e) {
        SHIELD_LOG_ERROR << "Failed to configure with Plugin system: "
                         << e.what();
        throw;
    }
}

void ExtensionContext::configure_with_annotations(
    core::ApplicationContext& ctx) {
    SHIELD_LOG_INFO
        << "Configuring ApplicationContext with annotation registry";
    annotations::ComponentRegistry::instance().auto_configure(ctx);
}

void ExtensionContext::configure_with_conditional_beans(
    core::ApplicationContext& ctx) {
    SHIELD_LOG_INFO
        << "Configuring ApplicationContext with conditional bean registry";
    conditions::ConditionalBeanRegistry::instance()
        .process_conditional_registrations(ctx);
}

void ExtensionContext::publish_application_started_event() {
    event_publisher_.emit_event<events::lifecycle::ApplicationStartedEvent>(
        std::string("ApplicationContext"));
}

void ExtensionContext::publish_application_stopped_event() {
    event_publisher_.emit_event<events::lifecycle::ApplicationStoppingEvent>(
        std::string("ApplicationContext"));
}

}  // namespace shield::extensions
