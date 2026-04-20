#include "shield/core/plugin_host.hpp"

#include "shield/core/application_context.hpp"
#include "shield/log/logger.hpp"

namespace shield::core {

void PluginHost::configure(ApplicationContext& context,
                           const std::string& plugins_directory) {
    auto plugin_manager = std::make_unique<PluginManager>();
    plugin_manager->add_plugin_directory(plugins_directory);
    configure(context, std::move(plugin_manager));
}

void PluginHost::configure(ApplicationContext& context,
                           std::unique_ptr<PluginManager> plugin_manager) {
    if (!plugin_manager) {
        throw std::invalid_argument("PluginHost requires a PluginManager");
    }

    SHIELD_LOG_INFO << "Configuring ApplicationContext with Plugin system";

    configure_plugin_events(*plugin_manager);

    size_t discovered = plugin_manager->discover_plugins();
    SHIELD_LOG_INFO << "Discovered " << discovered << " plugins";

    size_t loaded = plugin_manager->load_all_plugins();
    SHIELD_LOG_INFO << "Loaded " << loaded << " plugins";

    plugin_manager_ = std::move(plugin_manager);
    initialize_plugin_starters(context);

    SHIELD_LOG_INFO << "Successfully configured with Plugin system";
}

void PluginHost::initialize_plugin_starters(ApplicationContext& context) {
    auto plugin_starters = plugin_manager_->get_plugin_starters();
    for (auto* starter : plugin_starters) {
        if (!starter->is_enabled()) {
            continue;
        }

        try {
            SHIELD_LOG_INFO << "Initializing plugin starter: "
                            << starter->name();
            starter->pre_initialize(context);
            starter->initialize(context);
            starter->post_initialize(context);
            SHIELD_LOG_INFO << "Successfully initialized plugin starter: "
                            << starter->name();
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR << "Failed to initialize plugin starter "
                             << starter->name() << ": " << e.what();
            throw;
        }
    }
}

void PluginHost::configure_plugin_events(PluginManager& plugin_manager) {
    plugin_manager.set_event_callback(
        [](PluginManager::PluginEvent event, const std::string& plugin_name,
           const std::string& message) {
            switch (event) {
                case PluginManager::PluginEvent::DISCOVERED:
                    SHIELD_LOG_DEBUG << "Plugin discovered: " << plugin_name
                                     << " - " << message;
                    break;
                case PluginManager::PluginEvent::LOADING:
                    SHIELD_LOG_INFO << "Loading plugin: " << plugin_name
                                    << " - " << message;
                    break;
                case PluginManager::PluginEvent::LOADED:
                    SHIELD_LOG_INFO << "Plugin loaded successfully: "
                                    << plugin_name;
                    break;
                case PluginManager::PluginEvent::UNLOADING:
                    SHIELD_LOG_INFO << "Unloading plugin: " << plugin_name;
                    break;
                case PluginManager::PluginEvent::UNLOADED:
                    SHIELD_LOG_INFO << "Plugin unloaded: " << plugin_name;
                    break;
                case PluginManager::PluginEvent::ERROR:
                    SHIELD_LOG_ERROR << "Plugin error: " << plugin_name
                                     << " - " << message;
                    break;
            }
        });
}

}  // namespace shield::core
