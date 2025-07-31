#pragma once

#include <memory>
#include <sol/sol.hpp>
#include <string>
#include <unordered_map>

#include "shield/core/application_context.hpp"
#include "shield/events/event_system.hpp"
#include "shield/script/lua_engine.hpp"

namespace shield::lua {

/**
 * @brief C++ to Lua IOC Bridge
 *
 * This class bridges the C++ ApplicationContext with Lua's IOC container,
 * allowing seamless dependency injection between C++ and Lua services.
 */
class LuaIoCBridge {
public:
    LuaIoCBridge(core::ApplicationContext& cpp_context,
                 script::LuaEngine& lua_engine);

    /**
     * @brief Initialize the Lua IOC system
     */
    void initialize();

    /**
     * @brief Register C++ service to be accessible from Lua
     */
    template <typename T>
    void export_cpp_service(const std::string& name,
                            std::shared_ptr<T> service) {
        lua_state_["shield"]["cpp_services"][name] = service;

        // Register in Lua IoC container
        lua_state_.script(R"(
            shield.container:register_factory(")" +
                          name + R"(", function(container)
                return shield.cpp_services.)" +
                          name + R"(
            end, "singleton")
        )");
    }

    /**
     * @brief Register Lua service to be accessible from C++
     */
    void export_lua_service(const std::string& name,
                            const std::string& lua_service_name);

    /**
     * @brief Resolve service from Lua IoC container
     */
    sol::object resolve_lua_service(const std::string& name);

    /**
     * @brief Start Lua IOC container
     */
    void start_lua_container();

    /**
     * @brief Stop Lua IOC container
     */
    void stop_lua_container();

    /**
     * @brief Forward C++ events to Lua
     */
    void forward_cpp_event_to_lua(const std::string& event_type,
                                  const sol::object& event_data);

    /**
     * @brief Forward Lua events to C++
     */
    void forward_lua_event_to_cpp(const std::string& event_type,
                                  const sol::table& event_data);

    /**
     * @brief Setup bidirectional event forwarding
     */
    void setup_event_forwarding();

    /**
     * @brief Load and execute Lua IoC configuration script
     */
    void load_lua_ioc_script(const std::string& script_path);

    /**
     * @brief Get health status from Lua services
     */
    sol::table get_lua_health_status();

private:
    core::ApplicationContext& cpp_context_;
    script::LuaEngine& lua_engine_;
    sol::state& lua_state_;

    // Event forwarding maps
    std::unordered_map<std::string, std::function<void(const sol::table&)>>
        lua_to_cpp_handlers_;
    std::unordered_map<std::string, std::function<void(const sol::object&)>>
        cpp_to_lua_handlers_;

    void setup_lua_ioc_environment();
    void register_cpp_types();
    void setup_event_bridges();
};

/**
 * @brief Lua IOC Service wrapper for C++ services
 */
template <typename T>
class LuaServiceWrapper {
public:
    LuaServiceWrapper(std::shared_ptr<T> cpp_service) : service_(cpp_service) {}

    // Expose C++ service methods to Lua
    void bind_to_lua(sol::state& lua, const std::string& name) {
        lua[name] = service_;

        // If it's a Shield service, expose lifecycle methods
        if constexpr (std::is_base_of_v<core::Service, T>) {
            lua[name + "_start"] = [this]() {
                if (service_) service_->on_start();
            };

            lua[name + "_stop"] = [this]() {
                if (service_) service_->on_stop();
            };

            lua[name + "_name"] = [this]() -> std::string {
                return service_ ? service_->name() : "unknown";
            };
        }
    }

private:
    std::shared_ptr<T> service_;
};

/**
 * @brief C++ wrapper for Lua services
 */
class CppLuaServiceWrapper : public core::Service {
public:
    CppLuaServiceWrapper(const std::string& lua_service_name,
                         sol::state& lua_state, LuaIoCBridge& bridge);

    void on_init(core::ApplicationContext& ctx) override;
    void on_start() override;
    void on_stop() override;
    std::string name() const override;

    // Forward method calls to Lua service
    sol::object call_lua_method(const std::string& method_name,
                                sol::variadic_args args);

private:
    std::string lua_service_name_;
    sol::state& lua_state_;
    LuaIoCBridge& bridge_;
    sol::object lua_service_;
};

/**
 * @brief Event bridge for C++ <-> Lua event forwarding
 */
class LuaCppEventBridge {
public:
    LuaCppEventBridge(events::EventPublisher& cpp_publisher,
                      sol::state& lua_state);

    /**
     * @brief Forward C++ event to Lua
     */
    template <typename EventType>
    void forward_to_lua(const EventType& event) {
        try {
            // Convert C++ event to Lua table
            sol::table lua_event = lua_state_.create_table();
            lua_event["event_type"] = event.get_event_type();
            lua_event["timestamp"] =
                std::chrono::duration_cast<std::chrono::seconds>(
                    event.get_timestamp().time_since_epoch())
                    .count();

            // Add event-specific data
            if constexpr (std::is_same_v<
                              EventType,
                              events::lifecycle::ApplicationStartedEvent>) {
                lua_event["source"] = "cpp_application";
            } else if constexpr (std::is_same_v<
                                     EventType,
                                     events::config::ConfigRefreshEvent>) {
                lua_event["source"] = "cpp_config_manager";
            }

            // Publish to Lua IoC container
            lua_state_["shield"]["publish_event"](event.get_event_type(),
                                                  lua_event);

        } catch (const sol::error& e) {
            SHIELD_LOG_ERROR << "Failed to forward C++ event to Lua: "
                             << e.what();
        }
    }

    /**
     * @brief Forward Lua event to C++
     */
    void forward_to_cpp(const std::string& event_type,
                        const sol::table& lua_event);

private:
    events::EventPublisher& cpp_publisher_;
    sol::state& lua_state_;
};

}  // namespace shield::lua

/**
 * @brief Lua IoC integration macros
 */
#define SHIELD_EXPORT_TO_LUA(ServiceType, service_name) \
    lua_bridge.export_cpp_service<ServiceType>(         \
        #service_name, context.get_service<ServiceType>());

#define SHIELD_IMPORT_FROM_LUA(service_name, lua_service_name) \
    lua_bridge.export_lua_service(#service_name, #lua_service_name);