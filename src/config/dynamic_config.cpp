#include "shield/config/dynamic_config.hpp"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace shield::config {

DynamicConfigManager& DynamicConfigManager::instance() {
    static DynamicConfigManager instance;
    return instance;
}

void DynamicConfigManager::register_field(
    const std::string& module_name, const std::string& field_name,
    ConfigChangePolicy policy, const std::string& description,
    std::function<bool(const std::string&)> validator) {
    std::unique_lock lock(mutex_);

    ConfigFieldMetadata metadata{.name = field_name,
                                 .policy = policy,
                                 .description = description,
                                 .validator = validator};

    metadata_[module_name][field_name] = std::move(metadata);
}

template <typename T>
T DynamicConfigManager::get_config(const std::string& module_name,
                                   const std::string& field_name) const {
    std::shared_lock lock(mutex_);

    auto module_it = values_.find(module_name);
    if (module_it == values_.end()) {
        throw std::runtime_error("Module not found: " + module_name);
    }

    auto field_it = module_it->second.find(field_name);
    if (field_it == module_it->second.end()) {
        throw std::runtime_error("Field not found: " + module_name + "." +
                                 field_name);
    }

    const std::string& value_str = field_it->second;

    // 类型转换
    if constexpr (std::is_same_v<T, std::string>) {
        return value_str;
    } else if constexpr (std::is_same_v<T, int>) {
        return std::stoi(value_str);
    } else if constexpr (std::is_same_v<T, bool>) {
        return value_str == "true" || value_str == "1";
    } else if constexpr (std::is_same_v<T, double>) {
        return std::stod(value_str);
    } else {
        static_assert(sizeof(T) == 0, "Unsupported config type");
    }
}

template <typename T>
bool DynamicConfigManager::set_config(const std::string& module_name,
                                      const std::string& field_name,
                                      const T& value) {
    // 检查字段是否存在且为动态配置
    ConfigChangePolicy policy;
    {
        std::shared_lock lock(mutex_);
        auto module_it = metadata_.find(module_name);
        if (module_it == metadata_.end()) {
            return false;
        }

        auto field_it = module_it->second.find(field_name);
        if (field_it == module_it->second.end()) {
            return false;
        }

        policy = field_it->second.policy;
    }

    if (policy == ConfigChangePolicy::STATIC) {
        std::cerr << "Cannot modify static config: " << module_name << "."
                  << field_name << std::endl;
        return false;
    }

    // 转换为字符串
    std::string value_str;
    if constexpr (std::is_same_v<T, std::string>) {
        value_str = value;
    } else if constexpr (std::is_arithmetic_v<T>) {
        value_str = std::to_string(value);
    } else if constexpr (std::is_same_v<T, bool>) {
        value_str = value ? "true" : "false";
    } else {
        static_assert(sizeof(T) == 0, "Unsupported config type");
    }

    // 验证新值
    {
        std::shared_lock lock(mutex_);
        auto& field_metadata = metadata_[module_name][field_name];
        if (field_metadata.validator && !field_metadata.validator(value_str)) {
            std::cerr << "Config validation failed for " << module_name << "."
                      << field_name << " with value: " << value_str
                      << std::endl;
            return false;
        }
    }

    // 更新配置值
    std::string old_value;
    {
        std::unique_lock lock(mutex_);
        auto& module_values = values_[module_name];
        old_value = module_values[field_name];
        module_values[field_name] = value_str;
    }

    // 通知监听器
    notify_listeners(module_name, field_name, old_value, value_str);

    return true;
}

void DynamicConfigManager::add_listener(
    const std::string& module_name,
    std::shared_ptr<ConfigChangeListener> listener) {
    std::unique_lock lock(mutex_);
    listeners_[module_name].push_back(listener);
}

ConfigChangePolicy DynamicConfigManager::get_change_policy(
    const std::string& module_name, const std::string& field_name) const {
    std::shared_lock lock(mutex_);

    auto module_it = metadata_.find(module_name);
    if (module_it == metadata_.end()) {
        return ConfigChangePolicy::STATIC;
    }

    auto field_it = module_it->second.find(field_name);
    if (field_it == module_it->second.end()) {
        return ConfigChangePolicy::STATIC;
    }

    return field_it->second.policy;
}

std::vector<std::string> DynamicConfigManager::get_dynamic_fields(
    const std::string& module_name) const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> result;

    auto module_it = metadata_.find(module_name);
    if (module_it != metadata_.end()) {
        for (const auto& [field_name, metadata] : module_it->second) {
            if (metadata.policy == ConfigChangePolicy::DYNAMIC) {
                result.push_back(field_name);
            }
        }
    }

    return result;
}

void DynamicConfigManager::reload_dynamic_configs() {
    try {
        YAML::Node config = YAML::LoadFile("config/shield.yaml");

        std::unique_lock lock(mutex_);

        // 只重载动态配置字段
        for (const auto& [module_name, module_metadata] : metadata_) {
            if (config[module_name]) {
                for (const auto& [field_name, field_metadata] :
                     module_metadata) {
                    if (field_metadata.policy == ConfigChangePolicy::DYNAMIC) {
                        auto field_node = config[module_name][field_name];
                        if (field_node && field_node.IsScalar()) {
                            std::string old_value =
                                values_[module_name][field_name];
                            std::string new_value =
                                field_node.as<std::string>();

                            if (old_value != new_value) {
                                values_[module_name][field_name] = new_value;
                                lock.unlock();
                                notify_listeners(module_name, field_name,
                                                 old_value, new_value);
                                lock.lock();
                            }
                        }
                    }
                }
            }
        }

        std::cout << "Dynamic configs reloaded successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Failed to reload dynamic configs: " << e.what()
                  << std::endl;
    }
}

std::vector<std::string> DynamicConfigManager::get_all_modules() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> modules;

    for (const auto& [module_name, _] : metadata_) {
        modules.push_back(module_name);
    }

    return modules;
}

std::unordered_map<std::string, ConfigFieldMetadata>
DynamicConfigManager::get_module_metadata(
    const std::string& module_name) const {
    std::shared_lock lock(mutex_);

    auto it = metadata_.find(module_name);
    if (it != metadata_.end()) {
        return it->second;
    }

    return {};
}

std::vector<DynamicConfigManager::ConfigInfo>
DynamicConfigManager::get_all_config_info() const {
    std::shared_lock lock(mutex_);
    std::vector<ConfigInfo> result;

    for (const auto& [module_name, module_metadata] : metadata_) {
        for (const auto& [field_name, field_metadata] : module_metadata) {
            ConfigInfo info;
            info.module_name = module_name;
            info.field_name = field_name;
            info.policy = field_metadata.policy;
            info.description = field_metadata.description;

            // 获取当前值
            auto value_it = values_.find(module_name);
            if (value_it != values_.end()) {
                auto field_it = value_it->second.find(field_name);
                if (field_it != value_it->second.end()) {
                    info.current_value = field_it->second;
                }
            }

            result.push_back(info);
        }
    }

    return result;
}

bool DynamicConfigManager::batch_update_configs(
    const std::vector<ConfigUpdate>& updates) {
    std::vector<std::pair<std::string, std::string>> old_values;
    old_values.reserve(updates.size());

    // 验证所有更新
    {
        std::shared_lock lock(mutex_);
        for (const auto& update : updates) {
            // 检查字段是否存在
            auto module_it = metadata_.find(update.module_name);
            if (module_it == metadata_.end()) {
                std::cerr << "Module not found: " << update.module_name
                          << std::endl;
                return false;
            }

            auto field_it = module_it->second.find(update.field_name);
            if (field_it == module_it->second.end()) {
                std::cerr << "Field not found: " << update.module_name << "."
                          << update.field_name << std::endl;
                return false;
            }

            // 检查是否为动态配置
            if (field_it->second.policy == ConfigChangePolicy::STATIC) {
                std::cerr << "Cannot modify static config: "
                          << update.module_name << "." << update.field_name
                          << std::endl;
                return false;
            }

            // 验证新值
            if (field_it->second.validator &&
                !field_it->second.validator(update.value)) {
                std::cerr << "Validation failed for " << update.module_name
                          << "." << update.field_name
                          << " with value: " << update.value << std::endl;
                return false;
            }

            // 记录旧值
            auto value_it = values_.find(update.module_name);
            if (value_it != values_.end()) {
                auto old_value_it = value_it->second.find(update.field_name);
                if (old_value_it != value_it->second.end()) {
                    old_values.emplace_back(
                        update.module_name + "." + update.field_name,
                        old_value_it->second);
                } else {
                    old_values.emplace_back(
                        update.module_name + "." + update.field_name, "");
                }
            } else {
                old_values.emplace_back(
                    update.module_name + "." + update.field_name, "");
            }
        }
    }

    // 应用所有更新
    {
        std::unique_lock lock(mutex_);
        for (size_t i = 0; i < updates.size(); ++i) {
            const auto& update = updates[i];
            values_[update.module_name][update.field_name] = update.value;
        }
    }

    // 通知监听器
    for (size_t i = 0; i < updates.size(); ++i) {
        const auto& update = updates[i];
        const auto& old_value = old_values[i].second;
        notify_listeners(update.module_name, update.field_name, old_value,
                         update.value);
    }

    return true;
}

void DynamicConfigManager::notify_listeners(const std::string& module_name,
                                            const std::string& field_name,
                                            const std::string& old_value,
                                            const std::string& new_value) {
    std::shared_lock lock(mutex_);

    auto listeners_it = listeners_.find(module_name);
    if (listeners_it != listeners_.end()) {
        for (auto& listener : listeners_it->second) {
            if (listener) {
                listener->on_config_changed(field_name, old_value, new_value);
            }
        }
    }
}

std::string DynamicConfigManager::make_key(
    const std::string& module_name, const std::string& field_name) const {
    return module_name + "." + field_name;
}

// 显式实例化模板函数
template std::string DynamicConfigManager::get_config<std::string>(
    const std::string&, const std::string&) const;
template int DynamicConfigManager::get_config<int>(const std::string&,
                                                   const std::string&) const;
template bool DynamicConfigManager::get_config<bool>(const std::string&,
                                                     const std::string&) const;
template double DynamicConfigManager::get_config<double>(
    const std::string&, const std::string&) const;

template bool DynamicConfigManager::set_config<std::string>(const std::string&,
                                                            const std::string&,
                                                            const std::string&);
template bool DynamicConfigManager::set_config<int>(const std::string&,
                                                    const std::string&,
                                                    const int&);
template bool DynamicConfigManager::set_config<bool>(const std::string&,
                                                     const std::string&,
                                                     const bool&);
template bool DynamicConfigManager::set_config<double>(const std::string&,
                                                       const std::string&,
                                                       const double&);

}  // namespace shield::config