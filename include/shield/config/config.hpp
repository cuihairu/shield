// [SHIELD_CONFIG] Configuration module
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace shield::config {

/// @brief Config value type (can hold string, int, bool, double, array, object)
using ConfigValue = std::variant<
    std::string,
    int64_t,
    double,
    bool,
    std::vector<std::string>
>;

/// @brief Compile-time/module availability used by runtime config validation.
struct RuntimeValidationOptions {
    bool cluster_enabled = false;
    bool global_enabled = false;
    bool player_enabled = false;
    bool server_enabled = false;
    bool ops_enabled = false;
    bool require_actors = true;
};

/// @brief Actor declaration from the merged runtime config.
struct RuntimeActorConfig {
    std::string name;
    std::string script;
    std::string source_dir;
    int instances = 1;
    bool required = true;
    std::string options_json = "{}";
};

/// @brief Configuration interface
class Config {
public:
    Config();
    ~Config();

    // Load from YAML file
    bool load_yaml(std::string_view path);

    // Load from YAML string
    bool load_yaml_string(std::string_view yaml);

    // Get a value by key (supports dot notation: "database.host")
    // Returns default value if key not found
    std::string get_string(std::string_view key,
                          std::string_view default_value = "") const;

    int64_t get_int(std::string_view key,
                   int64_t default_value = 0) const;

    double get_double(std::string_view key,
                     double default_value = 0.0) const;

    bool get_bool(std::string_view key,
                 bool default_value = false) const;

    std::vector<std::string> get_string_array(
        std::string_view key) const;

    // Check if key exists
    bool has(std::string_view key) const;

    // Set a value (runtime override)
    void set(std::string_view key, ConfigValue value);

    // Get raw config value
    const ConfigValue* get_value(std::string_view key) const;

    // Merge another config (values from other take precedence)
    void merge(const Config& other);

    // Get config as JSON string (for Lua API)
    std::string to_json() const;

private:
    friend bool validate_runtime_config(const RuntimeValidationOptions& options,
                                        std::string* error);
    friend std::vector<RuntimeActorConfig> runtime_actors();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief Global config instance
Config& global_config();

/// @brief Initialize config from file
bool initialize_config(std::string_view config_path);

/// @brief Reload config from file
bool reload_config();

/// @brief Validate the loaded global runtime config against the Phase 1 schema.
bool validate_runtime_config(
    const RuntimeValidationOptions& options = RuntimeValidationOptions(),
    std::string* error = nullptr);

/// @brief Reset the global config snapshot.
void reset_config();

/// @brief Return actor declarations from the merged global config.
std::vector<RuntimeActorConfig> runtime_actors();

/// @brief Get a config value (convenience function)
/// Example: shield::config::get("database.host", "localhost")
std::string get(std::string_view key, std::string_view default_value = "");
int64_t get_int(std::string_view key, int64_t default_value = 0);
double get_double(std::string_view key, double default_value = 0.0);
bool get_bool(std::string_view key, bool default_value = false);

}  // namespace shield::config
