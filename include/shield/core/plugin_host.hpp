#pragma once

#include <memory>
#include <string>

#include "shield/core/plugin_manager.hpp"

namespace shield::core {

class ApplicationContext;

class PluginHost {
public:
    PluginHost() = default;

    void configure(ApplicationContext& context,
                   const std::string& plugins_directory);
    void configure(ApplicationContext& context,
                   std::unique_ptr<PluginManager> plugin_manager);

    PluginManager* get_plugin_manager() const { return plugin_manager_.get(); }

private:
    void initialize_plugin_starters(ApplicationContext& context);
    void configure_plugin_events(PluginManager& plugin_manager);

    std::unique_ptr<PluginManager> plugin_manager_;
};

}  // namespace shield::core
