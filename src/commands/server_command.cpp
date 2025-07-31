#include "shield/commands/server_command.hpp"

#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

#include "shield/config/application_configuration.hpp"
#include "shield/config/config.hpp"
#include "shield/core/all_starters.hpp"
#include "shield/core/application_context.hpp"
#include "shield/core/starter_manager.hpp"
#include "shield/fs/file_watcher.hpp"
#include "shield/version.hpp"

// Global flag for signal handling
extern volatile sig_atomic_t g_signal_status;
extern void signal_handler(int signal);

namespace shield::commands {

ServerCommand::ServerCommand()
    : shield::cli::Command("server", "Start the Shield game server") {
    setup_flags();
}

void ServerCommand::setup_flags() {
    add_flag("config", "Configuration file path", "config/shield.yaml");
    add_flag("plugins", "Plugins directory path", "plugins/");
    add_flag("enable-plugins", "Enable dynamic plugin loading", "false");
}

int ServerCommand::run(shield::cli::CommandContext& ctx) {
    std::cout << "Starting Shield Server..." << std::endl;

    // Load configuration
    auto& config_manager = shield::config::ConfigManager::instance();
    std::string config_file = ctx.get_flag("config");
    try {
        config_manager.load_config(config_file,
                                   shield::config::ConfigFormat::YAML);
        std::cout << "Loaded configuration from: " << config_file << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Failed to load configuration: " << e.what() << std::endl;
        return 1;
    }

    // Initialize ApplicationContext
    auto& app_context = shield::core::ApplicationContext::instance();

    // Check if plugins are enabled
    std::string enable_plugins = ctx.get_flag("enable-plugins");
    bool plugins_enabled = (enable_plugins == "true" || enable_plugins == "1");

    if (plugins_enabled) {
        // Configure with plugins
        std::string plugins_dir = ctx.get_flag("plugins");
        SHIELD_LOG_INFO << "Plugin system enabled, loading from: "
                        << plugins_dir;
        app_context.configure_with_plugins(plugins_dir);
    } else {
        // Create and configure StarterManager with all available Starters
        auto starter_manager = std::make_unique<shield::core::StarterManager>();

        // Register all Starters in the correct order (dependencies will be
        // resolved automatically)
        starter_manager->register_starter(
            std::make_unique<shield::script::ScriptStarter>());
        starter_manager->register_starter(
            std::make_unique<shield::actor::ActorStarter>());
        starter_manager->register_starter(
            std::make_unique<shield::metrics::MetricsStarter>());
        starter_manager->register_starter(
            std::make_unique<shield::gateway::GatewayStarter>());

        // Configure ApplicationContext using the Starter system
        app_context.configure_with_starters(std::move(starter_manager));
    }

    // Initialize and start all services
    app_context.init_all();
    app_context.start_all();

    // Start FileWatcher for dynamic config reloading
    auto config_watcher = std::make_unique<shield::fs::FileWatcher>();
    config_watcher->add_file(config_file);
    config_watcher->add_handler(
        [&config_manager, config_file](const shield::fs::FileEvent& event) {
            if (event.event_type == shield::fs::FileEventType::Modified) {
                try {
                    config_manager.reload_config(
                        config_file, shield::config::ConfigFormat::YAML);
                } catch (const std::exception& e) {
                    SHIELD_LOG_ERROR << "Failed to reload configuration: "
                                     << e.what();
                }
            }
        });
    config_watcher->start();

    SHIELD_LOG_INFO << "Application running. Press Ctrl+C to exit.";

    // Wait for termination signal
    while (g_signal_status == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    SHIELD_LOG_INFO << "Shutdown signal received. Stopping services...";

    // Stop all services
    app_context.stop_all();

    // Stop FileWatcher
    config_watcher->stop();

    SHIELD_LOG_INFO << "Application finished.";

    return 0;
}

}  // namespace shield::commands