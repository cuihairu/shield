#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "shield/fs/file_watcher.hpp"

namespace shield::business {

// Business configuration item
template <typename T>
class ConfigItem {
public:
    using ChangeCallback =
        std::function<void(const T& old_value, const T& new_value)>;

    ConfigItem(const std::string& key, const T& default_value)
        : key_(key), value_(default_value), default_value_(default_value) {}

    // Get current value
    const T& get() const { return value_; }

    // Set new value (will trigger callbacks)
    void set(const T& new_value) {
        if (new_value != value_) {
            T old_value = value_;
            value_ = new_value;

            for (const auto& callback : callbacks_) {
                callback(old_value, new_value);
            }
        }
    }

    // Add change callback
    void add_change_callback(ChangeCallback callback) {
        callbacks_.push_back(std::move(callback));
    }

    // Reset to default value
    void reset() { set(default_value_); }

    const std::string& key() const { return key_; }

private:
    std::string key_;
    T value_;
    T default_value_;
    std::vector<ChangeCallback> callbacks_;
};

// Configuration center interface
class ConfigCenter {
public:
    virtual ~ConfigCenter() = default;

    // Load configuration file
    virtual bool load_config(const std::string& config_file) = 0;

    // Get configuration values
    virtual std::string get_string(const std::string& key,
                                   const std::string& default_value = "") = 0;
    virtual int get_int(const std::string& key, int default_value = 0) = 0;
    virtual double get_double(const std::string& key,
                              double default_value = 0.0) = 0;
    virtual bool get_bool(const std::string& key,
                          bool default_value = false) = 0;

    // Set configuration values
    virtual void set_string(const std::string& key,
                            const std::string& value) = 0;
    virtual void set_int(const std::string& key, int value) = 0;
    virtual void set_double(const std::string& key, double value) = 0;
    virtual void set_bool(const std::string& key, bool value) = 0;

    // Register configuration change callbacks
    virtual void on_config_changed(const std::string& key,
                                   std::function<void()> callback) = 0;

    // Start file monitoring
    virtual void start_file_watching() = 0;

    // Stop file monitoring
    virtual void stop_file_watching() = 0;
};

// File-based configuration center implementation
class FileBasedConfigCenter : public ConfigCenter {
public:
    explicit FileBasedConfigCenter(const std::string& config_file);
    ~FileBasedConfigCenter() override;

    // ConfigCenter interface implementation
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

    // Additional features
    void add_config_file(const std::string& file_path);
    void remove_config_file(const std::string& file_path);
    std::vector<std::string> get_config_files() const;

    // Create strongly typed configuration item
    template <typename T>
    std::shared_ptr<ConfigItem<T>> create_config_item(const std::string& key,
                                                      const T& default_value) {
        auto item = std::make_shared<ConfigItem<T>>(key, default_value);

        // Load value from configuration immediately
        if constexpr (std::is_same_v<T, std::string>) {
            item->set(get_string(key, default_value));
        } else if constexpr (std::is_same_v<T, int>) {
            item->set(get_int(key, default_value));
        } else if constexpr (std::is_same_v<T, double>) {
            item->set(get_double(key, default_value));
        } else if constexpr (std::is_same_v<T, bool>) {
            item->set(get_bool(key, default_value));
        }

        // Register configuration change listener
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

    // Configuration storage
    std::unordered_map<std::string, std::string> config_values_;

    // Change callbacks
    std::unordered_map<std::string, std::vector<std::function<void()>>>
        change_callbacks_;

    mutable std::mutex mutex_;
};

// Configuration center manager
class ConfigCenterManager {
public:
    static ConfigCenterManager& instance();

    // Create configuration center
    std::shared_ptr<ConfigCenter> create_config_center(
        const std::string& name, const std::string& config_file);

    // Get configuration center
    std::shared_ptr<ConfigCenter> get_config_center(const std::string& name);

    // Remove configuration center
    void remove_config_center(const std::string& name);

    // Get all configuration center names
    std::vector<std::string> get_config_center_names() const;

    // Start file monitoring for all configuration centers
    void start_all_file_watching();

    // Stop file monitoring for all configuration centers
    void stop_all_file_watching();

private:
    ConfigCenterManager() = default;
    std::unordered_map<std::string, std::shared_ptr<ConfigCenter>>
        config_centers_;
    mutable std::mutex mutex_;
};

}  // namespace shield::business