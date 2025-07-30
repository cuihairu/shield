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
                     ConfigFormat format = ConfigFormat::YAML) override {
        ConfigManager::load_config(config_file, format);

        // 发布配置加载完成事件
        events::GlobalEventPublisher::emit<events::config::ConfigRefreshEvent>(
            std::string("ConfigManager"));
    }

    void reload_config(const std::string& config_file,
                       ConfigFormat format = ConfigFormat::YAML) override {
        SHIELD_LOG_INFO << "Attempting to reload config from: " << config_file;

        // 1. 解析新配置文件
        boost::property_tree::ptree new_config_tree;
        try {
            if (format == ConfigFormat::YAML) {
                YAML::Node yaml_node = YAML::LoadFile(config_file);
                new_config_tree = yaml_to_ptree(yaml_node);
            }
            // 其他格式支持...
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR
                << "Failed to parse new config file, aborting reload: "
                << e.what();
            return;
        }

        // 2. 事件驱动的配置更新
        std::unordered_map<std::type_index,
                           std::shared_ptr<ConfigurationProperties>>
            old_configs;
        std::unordered_map<std::type_index,
                           std::shared_ptr<ConfigurationProperties>>
            new_configs;

        try {
            std::lock_guard<std::mutex> lock(config_mutex_);

            // 保存旧配置的副本
            old_configs = configs_;

            for (auto const& [type_id, current_config] : configs_) {
                if (!current_config->supports_hot_reload()) {
                    continue;
                }

                auto new_config_clone = current_config->clone();
                const auto& properties_name =
                    new_config_clone->properties_name();

                // 从新配置树中加载
                new_config_clone->from_ptree(
                    new_config_tree.get_child(properties_name));
                new_config_clone->validate();

                new_configs[type_id] = std::move(new_config_clone);
            }
        } catch (const std::exception& e) {
            SHIELD_LOG_ERROR
                << "Failed to validate new configuration, aborting reload: "
                << e.what();
            return;
        }

        // 3. 原子性交换配置
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            config_tree_ = new_config_tree;
            for (auto const& [type_id, new_config] : new_configs) {
                configs_[type_id] = new_config;
                config_by_name_[new_config->properties_name()] = new_config;
            }
            SHIELD_LOG_INFO << "Successfully applied new configuration.";
        }

        // 4. 发布配置变更事件（类型安全的方式）
        publish_config_change_events(old_configs, new_configs);

        // 5. 发布通用刷新事件
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

    void publish_config_change_events(
        const std::unordered_map<std::type_index,
                                 std::shared_ptr<ConfigurationProperties>>&
            old_configs,
        const std::unordered_map<std::type_index,
                                 std::shared_ptr<ConfigurationProperties>>&
            new_configs) {
        for (const auto& [type_id, new_config] : new_configs) {
            auto old_it = old_configs.find(type_id);
            if (old_it != old_configs.end()) {
                // 发布类型安全的配置变更事件
                // 这里需要根据具体类型来发布相应的事件
                // 实际实现中可能需要使用注册表或模板特化

                SHIELD_LOG_INFO << "Publishing config change event for: "
                                << new_config->properties_name();

                // 通用配置刷新事件
                events::GlobalEventPublisher::emit<
                    events::config::ConfigRefreshEvent>(
                    new_config->properties_name());
            }
        }
    }
};

}  // namespace shield::config