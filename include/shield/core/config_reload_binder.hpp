#pragma once

#include <memory>

#include "shield/config/config.hpp"
#include "shield/core/service.hpp"

namespace shield::core {

class ConfigReloadBinder {
public:
    template <typename ConfigType>
    void bind(const std::shared_ptr<Service>& service) {
        auto reloadable = std::dynamic_pointer_cast<IReloadableService>(service);
        if (!reloadable) {
            throw std::runtime_error(
                "bind_config_reload requires an IReloadableService");
        }

        auto& config_manager = config::ConfigManager::instance();
        config_manager.subscribe_to_reloads<ConfigType>(
            [weak = std::weak_ptr<IReloadableService>(reloadable)](
                const ConfigType&) {
                if (auto locked = weak.lock()) {
                    locked->on_config_reloaded();
                }
            });
    }
};

}  // namespace shield::core
