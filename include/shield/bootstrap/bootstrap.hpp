// [SHIELD_BOOTSTRAP] Bootstrap and runtime entry point
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shield::bootstrap {

/// @brief Runtime configuration
struct RuntimeConfig {
    // Actor system config
    int num_workers = 0;  // 0 = auto (CPU cores)

    // Logging
    std::string log_level = "info";
    std::string log_file;  // Empty = console only

    // Config
    std::string config_file = "config/app.yaml";
    std::vector<std::string> config_files;
    std::string node_id;

    // Lua
    std::string lua_script_dir = "scripts";
};

/// @brief Initialize the Shield runtime
/// @param config Runtime configuration
/// @return true if initialization succeeded
bool initialize(const RuntimeConfig& config = RuntimeConfig());

/// @brief Shutdown the Shield runtime
void shutdown();

/// @brief Check if runtime is initialized
bool is_initialized();

/// @brief Main entry point for Shield applications
/// @param argc Argument count
/// @param argv Argument values
/// @return Exit code
int run(int argc, char** argv);

}  // namespace shield::bootstrap
