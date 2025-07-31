#pragma once

#include <boost/dll.hpp>
#include <memory>

#include "shield/core/starter.hpp"
#include "shield/log/logger.hpp"

/**
 * @brief Macros and utilities for creating Shield plugins
 *
 * This header provides convenience macros for plugin developers to
 * create and register their plugins with the Shield framework.
 */

namespace shield::core {

// Forward declaration from plugin_manager.hpp
struct PluginInfo;

/**
 * @brief Base class for plugin starters
 *
 * Plugin developers should inherit from this class instead of directly
 * implementing IStarter to get additional plugin-specific functionality.
 */
class PluginStarter : public IStarter {
public:
    virtual ~PluginStarter() = default;

    /**
     * @brief Get plugin metadata
     * @return Plugin info structure
     */
    virtual PluginInfo get_plugin_info() const = 0;
};

}  // namespace shield::core

/**
 * @brief Macro to declare plugin exports
 *
 * This macro should be used in the plugin source file to declare
 * the required export functions.
 *
 * Usage:
 * SHIELD_PLUGIN_EXPORTS(MyPluginStarter, "MyPlugin", "1.0.0", "Description",
 * "Author")
 */
#define SHIELD_PLUGIN_EXPORTS(StarterClass, Name, Version, Description, \
                              Author, ...)                              \
    extern "C" {                                                        \
    BOOST_DLL_ALIAS_SECTIONED(create_starter_impl, create_starter,      \
                              shield_plugin)                            \
    BOOST_DLL_ALIAS_SECTIONED(get_plugin_info_impl, get_plugin_info,    \
                              shield_plugin)                            \
    }                                                                   \
                                                                        \
    std::unique_ptr<shield::core::IStarter> create_starter_impl() {     \
        return std::make_unique<StarterClass>();                        \
    }                                                                   \
                                                                        \
    shield::core::PluginInfo get_plugin_info_impl() {                   \
        static const char* deps[] = {__VA_ARGS__, nullptr};             \
        return shield::core::PluginInfo{                                \
            Name, Version, Description, Author,                         \
            (sizeof..(__VA_ARGS__) > 0) ? deps : nullptr};              \
    }

/**
 * @brief Simplified macro for plugins without dependencies
 */
#define SHIELD_PLUGIN_SIMPLE(StarterClass, Name, Version, Description, Author) \
    SHIELD_PLUGIN_EXPORTS(StarterClass, Name, Version, Description, Author)

/**
 * @brief Macro for plugins with dependencies
 */
#define SHIELD_PLUGIN_WITH_DEPS(StarterClass, Name, Version, Description,   \
                                Author, ...)                                \
    SHIELD_PLUGIN_EXPORTS(StarterClass, Name, Version, Description, Author, \
                          __VA_ARGS__)

/**
 * @brief Plugin development helper macros
 */
#define SHIELD_PLUGIN_LOG_INFO(msg) \
    SHIELD_LOG_INFO << "[Plugin:" << name() << "] " << msg
#define SHIELD_PLUGIN_LOG_WARN(msg) \
    SHIELD_LOG_WARN << "[Plugin:" << name() << "] " << msg
#define SHIELD_PLUGIN_LOG_ERROR(msg) \
    SHIELD_LOG_ERROR << "[Plugin:" << name() << "] " << msg
#define SHIELD_PLUGIN_LOG_DEBUG(msg) \
    SHIELD_LOG_DEBUG << "[Plugin:" << name() << "] " << msg