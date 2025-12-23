// shield/src/script/lua_ioc_bridge.cpp
#include "shield/script/lua_ioc_bridge.hpp"

#include <stdexcept>

namespace shield::lua {

// =====================================
// LuaIoCBridge 实现
// =====================================

LuaIoCBridge::LuaIoCBridge(core::ApplicationContext& cpp_context,
                           script::LuaEngine& lua_engine)
    : cpp_context_(cpp_context),
      lua_engine_(lua_engine),
      lua_state_(lua_engine.lua()) {}

void LuaIoCBridge::initialize() {
    setup_lua_ioc_environment();
    register_cpp_types();
    setup_event_bridges();
}

void LuaIoCBridge::export_lua_service(const std::string& name,
                                      const std::string& lua_service_name) {
    // 从 Lua IoC 容器获取服务并注册到 C++ 上下文
    try {
        auto lua_service = lua_state_["shield"]["container"][lua_service_name];
        if (lua_service.valid()) {
            // 创建 C++ 包装器
            auto wrapper =
                std::make_shared<CppLuaServiceWrapper>(name, lua_state_, *this);

            // 注册到 C++ 上下文
            // cpp_context_.register_service(name, wrapper);

            std::cout << "[LuaIoCBridge] Exported Lua service '"
                      << lua_service_name << "' as '" << name << "' to C++"
                      << std::endl;
        }
    } catch (const sol::error& e) {
        std::cerr << "[LuaIoCBridge] Failed to export Lua service: " << e.what()
                  << std::endl;
    }
}

sol::object LuaIoCBridge::resolve_lua_service(const std::string& name) {
    try {
        return lua_state_["shield"]["container"]["resolve"](name);
    } catch (const sol::error& e) {
        std::cerr << "[LuaIoCBridge] Failed to resolve Lua service '" << name
                  << "': " << e.what() << std::endl;
        return sol::lua_nil;
    }
}

void LuaIoCBridge::start_lua_container() {
    try {
        lua_state_.script(R"(
            if shield.container and shield.container.start then
                shield.container:start()
            end
        )");

        std::cout << "[LuaIoCBridge] Lua IoC container started" << std::endl;
    } catch (const sol::error& e) {
        std::cerr << "[LuaIoCBridge] Failed to start Lua container: "
                  << e.what() << std::endl;
    }
}

void LuaIoCBridge::stop_lua_container() {
    try {
        lua_state_.script(R"(
            if shield.container and shield.container.stop then
                shield.container:stop()
            end
        )");

        std::cout << "[LuaIoCBridge] Lua IoC container stopped" << std::endl;
    } catch (const sol::error& e) {
        std::cerr << "[LuaIoCBridge] Failed to stop Lua container: " << e.what()
                  << std::endl;
    }
}

void LuaIoCBridge::forward_cpp_event_to_lua(const std::string& event_type,
                                            const sol::object& event_data) {
    try {
        // 调用 Lua 事件处理器
        auto handler = lua_state_["shield"]["handle_cpp_event"];
        if (handler.valid()) {
            handler(event_type, event_data);
        }
    } catch (const sol::error& e) {
        std::cerr << "[LuaIoCBridge] Failed to forward C++ event to Lua: "
                  << e.what() << std::endl;
    }
}

void LuaIoCBridge::forward_lua_event_to_cpp(const std::string& event_type,
                                            const sol::table& event_data) {
    // 查找对应的 C++ 事件处理器并调用
    auto it = lua_to_cpp_handlers_.find(event_type);
    if (it != lua_to_cpp_handlers_.end() && it->second) {
        try {
            it->second(event_data);
        } catch (const std::exception& e) {
            std::cerr << "[LuaIoCBridge] Error in Lua->CPP event handler: "
                      << e.what() << std::endl;
        }
    }
}

void LuaIoCBridge::setup_event_forwarding() {
    // 设置 C++ 到 Lua 的事件转发
    // 注册事件监听器到 C++ 事件发布器

    // 设置 Lua 到 C++ 的事件转发
    lua_state_.script(R"(
        function shield.publish_event_to_cpp(event_type, event_data)
            -- 这个函数会被 Lua 代码调用来发布事件到 C++
        end
    )");
}

void LuaIoCBridge::load_lua_ioc_script(const std::string& script_path) {
    try {
        lua_state_.script_file(script_path);
        std::cout << "[LuaIoCBridge] Loaded Lua IoC script: " << script_path
                  << std::endl;
    } catch (const sol::error& e) {
        std::cerr << "[LuaIoCBridge] Failed to load Lua IoC script: "
                  << e.what() << std::endl;
        throw std::runtime_error("Failed to load Lua IoC script: " +
                                 std::string(e.what()));
    }
}

sol::table LuaIoCBridge::get_lua_health_status() {
    try {
        sol::table health = lua_state_.create_table();

        // 获取 Lua 容器中所有服务的健康状态
        if (lua_state_["shield"]["container"]["get_all_services"].valid()) {
            auto services_result =
                lua_state_["shield"]["container"]["get_all_services"]();

            if (services_result.valid()) {
                // services_result 应该是一个 table
                auto services = services_result.get<sol::table>();
                if (services) {
                    for (const auto& [name, service] : services) {
                        sol::table service_health = lua_state_.create_table();
                        service_health["name"] = name;

                        // 检查服务是否有健康检查方法
                        auto service_table = service.as<sol::table>();
                        if (service_table &&
                            service_table["health_check"].valid()) {
                            try {
                                auto result = service_table["health_check"](
                                    service_table);
                                if (result.valid()) {
                                    service_health["status"] = result;
                                } else {
                                    service_health["status"] = "error";
                                }
                            } catch (const std::exception&) {
                                service_health["status"] = "error";
                            }
                        } else {
                            service_health["status"] = "ok";
                        }

                        health[name] = service_health;
                    }
                }
            }
        }

        return health;

    } catch (const sol::error& e) {
        std::cerr << "[LuaIoCBridge] Failed to get Lua health status: "
                  << e.what() << std::endl;

        sol::table error_table = lua_state_.create_table();
        error_table["error"] = e.what();
        return error_table;
    }
}

void LuaIoCBridge::setup_lua_ioc_environment() {
    // 设置 Lua IoC 容器环境
    lua_state_.script(R"(
        shield = shield or {}
        shield.container = {
            services = {},
            factories = {},
            singletons = {},

            register = function(self, name, factory, lifetime)
                lifetime = lifetime or "singleton"
                table.insert(self.factories, {
                    name = name,
                    factory = factory,
                    lifetime = lifetime
                })
            end,

            register_factory = function(self, name, factory, lifetime)
                return self:register(name, factory, lifetime)
            end,

            resolve = function(self, name)
                -- 首先检查单例缓存
                if self.singletons[name] then
                    return self.singletons[name]
                end

                -- 查找工厂
                for _, info in ipairs(self.factories) do
                    if info.name == name then
                        local instance = info.factory(self)

                        -- 如果是单例，缓存它
                        if info.lifetime == "singleton" then
                            self.singletons[name] = instance
                        end

                        return instance
                    end
                end

                error("Service not found: " .. name)
            end,

            start = function(self)
                -- 初始化所有延迟加载的服务
                print("Lua IoC container started")
            end,

            stop = function(self)
                -- 清理所有服务
                self.singletons = {}
                print("Lua IoC container stopped")
            end,

            get_all_services = function(self)
                local all = {}
                for name, _ in pairs(self.singletons) do
                    all[name] = self.singletons[name]
                end
                return all
            end
        }
    )");
}

void LuaIoCBridge::register_cpp_types() {
    // 注册 C++ 类型到 Lua
    lua_state_.new_usertype<core::Service>(
        "Service", "name", &core::Service::name, "on_start",
        &core::Service::on_start, "on_stop", &core::Service::on_stop);
}

void LuaIoCBridge::setup_event_bridges() {
    // 设置事件桥接
    lua_state_.script(R"(
        shield.events = shield.events or {}
        shield.events.handlers = {}

        function shield.events.subscribe(event_type, handler)
            if not shield.events.handlers[event_type] then
                shield.events.handlers[event_type] = {}
            end
            table.insert(shield.events.handlers[event_type], handler)
        end

        function shield.events.publish(event_type, event_data)
            local handlers = shield.events.handlers[event_type]
            if handlers then
                for _, handler in ipairs(handlers) do
                    handler(event_data)
                end
            end

            -- 转发到 C++
            shield.publish_event_to_cpp(event_type, event_data)
        end
    )");
}

// =====================================
// CppLuaServiceWrapper 实现
// =====================================

CppLuaServiceWrapper::CppLuaServiceWrapper(const std::string& lua_service_name,
                                           sol::state& lua_state,
                                           LuaIoCBridge& bridge)
    : lua_service_name_(lua_service_name),
      lua_state_(lua_state),
      bridge_(bridge) {}

void CppLuaServiceWrapper::on_init(core::ApplicationContext& ctx) {
    try {
        auto lua_service =
            lua_state_["shield"]["container"]["resolve"](lua_service_name_);

        if (lua_service.valid()) {
            lua_service_ = lua_service;

            // 调用 Lua 服务的初始化方法（如果有）
            if (lua_service_["on_init"].valid()) {
                lua_service_["on_init"](lua_service_);
            }
        }
    } catch (const sol::error& e) {
        std::cerr << "[CppLuaServiceWrapper] Failed to initialize: " << e.what()
                  << std::endl;
    }
}

void CppLuaServiceWrapper::on_start() {
    try {
        if (lua_service_.valid() && lua_service_["on_start"].valid()) {
            lua_service_["on_start"](lua_service_);
        }
    } catch (const sol::error& e) {
        std::cerr << "[CppLuaServiceWrapper] Failed to start: " << e.what()
                  << std::endl;
    }
}

void CppLuaServiceWrapper::on_stop() {
    try {
        if (lua_service_.valid() && lua_service_["on_stop"].valid()) {
            lua_service_["on_stop"](lua_service_);
        }
    } catch (const sol::error& e) {
        std::cerr << "[CppLuaServiceWrapper] Failed to stop: " << e.what()
                  << std::endl;
    }
}

std::string CppLuaServiceWrapper::name() const {
    return "LuaService:" + lua_service_name_;
}

sol::object CppLuaServiceWrapper::call_lua_method(
    const std::string& method_name, sol::variadic_args args) {
    try {
        if (lua_service_.valid() && lua_service_[method_name].valid()) {
            auto method = lua_service_[method_name];
            return method(lua_service_, args);
        }
        return sol::lua_nil;
    } catch (const sol::error& e) {
        std::cerr << "[CppLuaServiceWrapper] Failed to call method '"
                  << method_name << "': " << e.what() << std::endl;
        return sol::lua_nil;
    }
}

// =====================================
// LuaCppEventBridge 实现
// =====================================

LuaCppEventBridge::LuaCppEventBridge(events::EventPublisher& cpp_publisher,
                                     sol::state& lua_state)
    : cpp_publisher_(cpp_publisher), lua_state_(lua_state) {}

void LuaCppEventBridge::forward_to_cpp(const std::string& event_type,
                                       const sol::table& lua_event) {
    // 这里需要根据 event_type 创建对应的 C++ 事件对象
    // 然后通过 cpp_publisher_ 发布

    if (event_type == "ApplicationStartedEvent") {
        // cpp_publisher_.publish_event(
        //     std::make_shared<events::lifecycle::ApplicationStartedEvent>());
    } else if (event_type == "ConfigRefreshEvent") {
        // cpp_publisher_.publish_event(
        //     std::make_shared<events::config::ConfigRefreshEvent>());
    }
}

}  // namespace shield::lua
