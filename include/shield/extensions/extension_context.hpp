// [OPTIONAL]
#pragma once

#include <memory>
#include <string>

#include "shield/core/application_context.hpp"
#include "shield/core/plugin_host.hpp"
#include "shield/di/advanced_container.hpp"
#include "shield/events/event_publisher.hpp"

namespace shield::extensions {

class ExtensionContext {
public:
    static ExtensionContext& instance();

    di::AdvancedContainer& get_di_container() { return di_container_; }
    events::DefaultEventPublisher& get_event_publisher() {
        return event_publisher_;
    }

    void configure_with_plugins(core::ApplicationContext& ctx,
                                const std::string& plugins_directory);
    void configure_with_plugins(
        core::ApplicationContext& ctx,
        std::unique_ptr<core::PluginManager> plugin_manager);

    void configure_with_annotations(core::ApplicationContext& ctx);
    void configure_with_conditional_beans(core::ApplicationContext& ctx);

    void publish_application_started_event();
    void publish_application_stopped_event();

private:
    ExtensionContext() = default;

    di::AdvancedContainer di_container_;
    events::DefaultEventPublisher event_publisher_;
    std::unique_ptr<core::PluginHost> plugin_host_;
};

}  // namespace shield::extensions
