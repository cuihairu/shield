#include "shield/core/application_context.hpp"
#include "shield/core/plugin_manager.hpp"
#include "shield/core/plugin_utils.hpp"
#include "shield/log/logger.hpp"

/**
 * @brief Example plugin that demonstrates the plugin system
 *
 * This plugin registers a simple service that logs messages
 * and provides an example of how to create Shield plugins.
 */
class ExamplePluginStarter : public shield::core::PluginStarter {
public:
    void initialize(shield::core::ApplicationContext& context) override {
        SHIELD_PLUGIN_LOG_INFO("Initializing Example Plugin");

        // This is where the plugin would register its services
        // For demonstration, we'll just log a message
        SHIELD_PLUGIN_LOG_INFO("Example Plugin initialized successfully");
        SHIELD_PLUGIN_LOG_INFO(
            "This plugin demonstrates how to create Shield plugins");
    }

    std::string name() const override { return "ExamplePlugin"; }

    std::vector<std::string> depends_on() const override {
        // Example plugin has no dependencies
        return {};
    }

    bool is_enabled() const override {
        // Plugin is always enabled by default
        return true;
    }

    void pre_initialize(shield::core::ApplicationContext& context) override {
        SHIELD_PLUGIN_LOG_DEBUG("Pre-initializing Example Plugin");
    }

    void post_initialize(shield::core::ApplicationContext& context) override {
        SHIELD_PLUGIN_LOG_DEBUG("Post-initializing Example Plugin");
    }

    shield::core::PluginInfo get_plugin_info() const override {
        return shield::core::PluginInfo{
            "ExamplePlugin", "1.0.0",
            "An example plugin demonstrating the Shield plugin system",
            "Shield Framework",
            nullptr  // no dependencies
        };
    }
};

// Export the plugin using the convenience macro
SHIELD_PLUGIN_SIMPLE(ExamplePluginStarter, "ExamplePlugin", "1.0.0",
                     "An example plugin demonstrating the Shield plugin system",
                     "Shield Framework")