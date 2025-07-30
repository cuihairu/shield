#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "shield/fs/file_watcher.hpp"

namespace shield::business {

// 业务配置项
template <typename T>
class ConfigItem {
public:
    using ChangeCallback =
        std::function<void(const T& old_value, const T& new_value)>;

    ConfigItem(const std::string& key, const T& default_value)
        : key_(key), value_(default_value), default_value_(default_value) {}

    // 获取当前值
    const T& get() const { return value_; }

    // 设置新值（会触发回调）
    void set(const T& new_value) {
        if (new_value != value_) {
            T old_value = value_;
            value_ = new_value;

            for (const auto& callback : callbacks_) {
                callback(old_value, new_value);
            }
        }
    }

    // 添加变更回调
    void add_change_callback(ChangeCallback callback) {
        callbacks_.push_back(std::move(callback));
    }

    // 重置为默认值
    void reset() { set(default_value_); }

    const std::string& key() const { return key_; }

private:
    std::string key_;
    T value_;
    T default_value_;
    std::vector<ChangeCallback> callbacks_;
};

// 配置中心接口
class ConfigCenter {
public:
    virtual ~ConfigCenter() = default;

    // 加载配置文件
    virtual bool load_config(const std::string& config_file) = 0;

    // 获取配置值
    virtual std::string get_string(const std::string& key,
                                   const std::string& default_value = "") = 0;
    virtual int get_int(const std::string& key, int default_value = 0) = 0;
    virtual double get_double(const std::string& key,
                              double default_value = 0.0) = 0;
    virtual bool get_bool(const std::string& key,
                          bool default_value = false) = 0;

    // 设置配置值
    virtual void set_string(const std::string& key,
                            const std::string& value) = 0;
    virtual void set_int(const std::string& key, int value) = 0;
    virtual void set_double(const std::string& key, double value) = 0;
    virtual void set_bool(const std::string& key, bool value) = 0;

    // 注册配置变更回调
    virtual void on_config_changed(const std::string& key,
                                   std::function<void()> callback) = 0;

    // 开始文件监控
    virtual void start_file_watching() = 0;

    // 停止文件监控
    virtual void stop_file_watching() = 0;
};

// 基于文件的配置中心实现
class FileBasedConfigCenter : public ConfigCenter {
public:
    explicit FileBasedConfigCenter(const std::string& config_file);
    ~FileBasedConfigCenter() override;

    // ConfigCenter接口实现
    bool load_config(const std::string& config_file) override;
    std::string get_string(const std::string& key,
                           const std::string& default_value = "") override;
    int get_int(const std::string& key, int default_value = 0) override;
    double get_double(const std::string& key,
                      double default_value = 0.0) override;
    bool get_bool(const std::string& key, bool default_value = false) override;

    void set_string(const std::string& key, const std::string& value) override;
    void set_int(const std::string& key, int value) override;
    void set_double(const std::string& key, double value) override;
    void set_bool(const std::string& key, bool value) override;

    void on_config_changed(const std::string& key,
                           std::function<void()> callback) override;
    void start_file_watching() override;
    void stop_file_watching() override;

    // 额外功能
    void add_config_file(const std::string& file_path);
    void remove_config_file(const std::string& file_path);
    std::vector<std::string> get_config_files() const;

    // 创建强类型配置项
    template <typename T>
    std::shared_ptr<ConfigItem<T>> create_config_item(const std::string& key,
                                                      const T& default_value) {
        auto item = std::make_shared<ConfigItem<T>>(key, default_value);

        // 立即从配置中加载值
        if constexpr (std::is_same_v<T, std::string>) {
            item->set(get_string(key, default_value));
        } else if constexpr (std::is_same_v<T, int>) {
            item->set(get_int(key, default_value));
        } else if constexpr (std::is_same_v<T, double>) {
            item->set(get_double(key, default_value));
        } else if constexpr (std::is_same_v<T, bool>) {
            item->set(get_bool(key, default_value));
        }

        // 注册配置变更监听
        on_config_changed(key, [this, item, key]() {
            if constexpr (std::is_same_v<T, std::string>) {
                item->set(get_string(key, item->get()));
            } else if constexpr (std::is_same_v<T, int>) {
                item->set(get_int(key, item->get()));
            } else if constexpr (std::is_same_v<T, double>) {
                item->set(get_double(key, item->get()));
            } else if constexpr (std::is_same_v<T, bool>) {
                item->set(get_bool(key, item->get()));
            }
        });

        return item;
    }

private:
    void on_file_changed(const shield::fs::FileEvent& event);
    void reload_all_configs();

    std::string primary_config_file_;
    std::vector<std::string> config_files_;
    std::unique_ptr<shield::fs::FileWatcher> file_watcher_;

    // 配置存储
    std::unordered_map<std::string, std::string> config_values_;

    // 变更回调
    std::unordered_map<std::string, std::vector<std::function<void()>>>
        change_callbacks_;

    mutable std::mutex mutex_;
};

// 配置中心管理器
class ConfigCenterManager {
public:
    static ConfigCenterManager& instance();

    // 创建配置中心
    std::shared_ptr<ConfigCenter> create_config_center(
        const std::string& name, const std::string& config_file);

    // 获取配置中心
    std::shared_ptr<ConfigCenter> get_config_center(const std::string& name);

    // 移除配置中心
    void remove_config_center(const std::string& name);

    // 获取所有配置中心名称
    std::vector<std::string> get_config_center_names() const;

    // 启动所有配置中心的文件监控
    void start_all_file_watching();

    // 停止所有配置中心的文件监控
    void stop_all_file_watching();

private:
    ConfigCenterManager() = default;
    std::unordered_map<std::string, std::shared_ptr<ConfigCenter>>
        config_centers_;
    mutable std::mutex mutex_;
};

}  // namespace shield::business