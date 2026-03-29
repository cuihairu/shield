#pragma once

#include "shield/config/config.hpp"
#include "shield/events/event_publisher.hpp"
#include "shield/events/event_system.hpp"

namespace shield::config {

// 事件驱动的配置管理器
class EventDrivenConfigManager : public ConfigManager {
public:
    static EventDrivenConfigManager& instance() {
        static EventDrivenConfigManager instance;
        return instance;
    }

    // 重载配置加载方法，加入事件发布
    void load_config(const std::string& config_file,
                     ConfigFormat format = ConfigFormat::YAML) {
        ConfigManager::load_config(config_file, format);

        // 发布配置加载完成事件
        events::GlobalEventPublisher::emit<events::config::ConfigRefreshEvent>(
            std::string("ConfigManager"));
    }

    void reload_config(const std::string& config_file,
                       ConfigFormat format = ConfigFormat::YAML) {
        ConfigManager::reload_config(config_file, format);

        // 发布通用刷新事件
        events::GlobalEventPublisher::emit<events::config::ConfigRefreshEvent>(
            std::string("ConfigManager"));
    }

    // 注册配置属性时发布绑定事件
    template <typename T>
    void register_configuration_properties(std::shared_ptr<T> config) {
        ConfigManager::register_configuration_properties(config);

        // 发布配置属性绑定事件
        events::GlobalEventPublisher::emit<
            events::config::ConfigPropertiesBindEvent<T>>(
            config, std::string("ConfigManager"));
    }

private:
    EventDrivenConfigManager() = default;
};

}  // namespace shield::config
