// [SHIELD_LUA] Lua Runtime implementation
#include "shield/lua/lua_runtime.hpp"

#include "shield/lua/lua_api.hpp"
#include "shield/lua/lua_service.hpp"

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <algorithm>

namespace shield::lua {

// Opaque Lua VM handle
class LuaVM {
public:
    LuaVM() : state_(std::make_shared<sol::state>()) {
        state_->open_libraries(sol::lib::base, sol::lib::string,
                               sol::lib::table, sol::lib::math,
                               sol::lib::io, sol::lib::os);
    }

    std::shared_ptr<sol::state> state() { return state_; }
    sol::table& service_table() { return service_table_; }
    void service_table(sol::table table) { service_table_ = std::move(table); }

private:
    std::shared_ptr<sol::state> state_;
    sol::table service_table_ = sol::nil;
};

struct LuaRuntime::Impl {
    std::shared_ptr<sol::state> default_state;
    LuaServiceManager* service_manager = nullptr;

    Impl() : default_state(std::make_shared<sol::state>()) {
        default_state->open_libraries(sol::lib::base, sol::lib::string,
                                     sol::lib::table, sol::lib::math);
    }
};

LuaRuntime::LuaRuntime() : impl_(std::make_unique<Impl>()) {}

LuaRuntime::~LuaRuntime() = default;

std::shared_ptr<LuaVM> LuaRuntime::create_vm() {
    return std::make_shared<LuaVM>();
}

bool LuaRuntime::load_script(std::shared_ptr<LuaVM> vm,
                            std::string_view script_path) {
    try {
        auto result = vm->state()->script_file(std::string(script_path));
        return result.valid();
    } catch (const std::exception& e) {
        return false;
    }
}

namespace {

sol::object json_to_lua(sol::state_view lua, const nlohmann::json& value) {
    if (value.is_null()) {
        return sol::make_object(lua, sol::nil);
    }
    if (value.is_boolean()) {
        return sol::make_object(lua, value.get<bool>());
    }
    if (value.is_number_integer()) {
        return sol::make_object(lua, value.get<std::int64_t>());
    }
    if (value.is_number_unsigned()) {
        return sol::make_object(lua, value.get<std::uint64_t>());
    }
    if (value.is_number_float()) {
        return sol::make_object(lua, value.get<double>());
    }
    if (value.is_string()) {
        return sol::make_object(lua, value.get<std::string>());
    }
    if (value.is_array()) {
        sol::table table = lua.create_table();
        int index = 1;
        for (const auto& item : value) {
            table[index++] = json_to_lua(lua, item);
        }
        return sol::make_object(lua, table);
    }
    if (value.is_object()) {
        sol::table table = lua.create_table();
        for (const auto& [key, item] : value.items()) {
            table[key] = json_to_lua(lua, item);
        }
        return sol::make_object(lua, table);
    }
    return sol::make_object(lua, sol::nil);
}

bool lua_to_json(const sol::object& value, nlohmann::json* out) {
    if (!out) {
        return true;
    }

    if (!value.valid() || value == sol::nil) {
        *out = nullptr;
        return true;
    }
    if (value.is<bool>()) {
        *out = value.as<bool>();
        return true;
    }
    if (value.is<std::int64_t>()) {
        *out = value.as<std::int64_t>();
        return true;
    }
    if (value.is<int>()) {
        *out = value.as<int>();
        return true;
    }
    if (value.is<double>()) {
        *out = value.as<double>();
        return true;
    }
    if (value.is<std::string>()) {
        *out = value.as<std::string>();
        return true;
    }
    if (!value.is<sol::table>()) {
        *out = "<unsupported>";
        return false;
    }

    sol::table table = value.as<sol::table>();
    bool array_like = true;
    std::size_t max_index = 0;
    std::size_t entry_count = 0;
    for (const auto& [key, _] : table) {
        ++entry_count;
        sol::object key_obj = key;
        if (!key_obj.is<int>()) {
            array_like = false;
            break;
        }
        const int index = key_obj.as<int>();
        if (index <= 0) {
            array_like = false;
            break;
        }
        max_index = std::max(max_index, static_cast<std::size_t>(index));
    }

    if (array_like && max_index == entry_count) {
        nlohmann::json array = nlohmann::json::array();
        for (std::size_t i = 1; i <= max_index; ++i) {
            nlohmann::json item;
            if (!lua_to_json(table[static_cast<int>(i)], &item)) {
                return false;
            }
            array.push_back(std::move(item));
        }
        *out = std::move(array);
        return true;
    }

    nlohmann::json object = nlohmann::json::object();
    for (const auto& [key, val] : table) {
        sol::object key_obj = key;
        std::string object_key;
        if (key_obj.is<std::string>()) {
            object_key = key_obj.as<std::string>();
        } else if (key_obj.is<int>()) {
            object_key = std::to_string(key_obj.as<int>());
        } else {
            continue;
        }

        nlohmann::json item;
        if (!lua_to_json(val, &item)) {
            return false;
        }
        object[object_key] = std::move(item);
    }
    *out = std::move(object);
    return true;
}

}  // namespace

bool LuaRuntime::load_service_module(std::shared_ptr<LuaVM> vm,
                                     std::string_view script_path,
                                     std::string* error) {
    try {
        sol::state& lua = *vm->state();
        sol::load_result loaded = lua.load_file(std::string(script_path));
        if (!loaded.valid()) {
            sol::error err = loaded;
            if (error) {
                *error = err.what();
            }
            return false;
        }

        sol::protected_function chunk = loaded;
        sol::protected_function_result result = chunk();
        if (!result.valid()) {
            sol::error err = result;
            if (error) {
                *error = err.what();
            }
            return false;
        }

        sol::object module = result;
        if (!module.is<sol::table>()) {
            if (error) {
                *error = "service module must return a table";
            }
            return false;
        }

        vm->service_table(module.as<sol::table>());
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

bool LuaRuntime::call_service_function(std::shared_ptr<LuaVM> vm,
                                       std::string_view func_name,
                                       const nlohmann::json& args,
                                       std::string* error) {
    try {
        sol::table& service = vm->service_table();
        if (!service.valid()) {
            if (error) {
                *error = "service module not loaded";
            }
            return false;
        }

        sol::object value = service[std::string(func_name)];
        if (!value.valid() || value == sol::nil) {
            return true;
        }
        if (!value.is<sol::protected_function>()) {
            if (error) {
                *error = std::string(func_name) + " is not a function";
            }
            return false;
        }

        sol::state_view lua(*vm->state());
        sol::protected_function func = value.as<sol::protected_function>();
        sol::protected_function_result result = func(json_to_lua(lua, args));
        if (!result.valid()) {
            sol::error err = result;
            if (error) {
                *error = err.what();
            }
            return false;
        }

        sol::object first = result.get<sol::object>(0);
        if (first.is<bool>() && !first.as<bool>()) {
            sol::object second = result.get<sol::object>(1);
            if (error) {
                *error = second.valid() && second != sol::nil
                             ? second.as<std::string>()
                             : std::string(func_name) + " returned false";
            }
            return false;
        }
        if (first == sol::nil && result.return_count() > 1) {
            sol::object second = result.get<sol::object>(1);
            if (error) {
                *error = second.valid() && second != sol::nil
                             ? second.as<std::string>()
                             : std::string(func_name) + " returned nil";
            }
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

bool LuaRuntime::call_service_method(std::shared_ptr<LuaVM> vm,
                                     std::string_view method_name,
                                     const nlohmann::json& args,
                                     nlohmann::json* returns,
                                     std::string* error) {
    try {
        sol::table& service = vm->service_table();
        if (!service.valid()) {
            if (error) {
                *error = "service module not loaded";
            }
            return false;
        }

        sol::object value = service[std::string(method_name)];
        if (!value.valid() || value == sol::nil) {
            if (error) {
                *error = "method not found: " + std::string(method_name);
            }
            return false;
        }
        if (!value.is<sol::protected_function>()) {
            if (error) {
                *error = std::string(method_name) + " is not a function";
            }
            return false;
        }

        if (!args.is_array()) {
            if (error) {
                *error = "method args must be a JSON array";
            }
            return false;
        }

        sol::state_view lua(*vm->state());
        std::vector<sol::object> lua_args;
        lua_args.reserve(args.size());
        for (const auto& arg : args) {
            lua_args.push_back(json_to_lua(lua, arg));
        }

        sol::protected_function func = value.as<sol::protected_function>();
        sol::protected_function_result result = func(sol::as_args(lua_args));
        if (!result.valid()) {
            sol::error err = result;
            if (error) {
                *error = err.what();
            }
            return false;
        }

        if (returns) {
            *returns = nlohmann::json::array();
            for (int i = 0; i < result.return_count(); ++i) {
                nlohmann::json item;
                if (!lua_to_json(result.get<sol::object>(i), &item)) {
                    if (error) {
                        *error = "unsupported return value from " +
                                 std::string(method_name);
                    }
                    return false;
                }
                returns->push_back(std::move(item));
            }
        }

        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

std::string LuaRuntime::call_function(std::shared_ptr<LuaVM> vm,
                                     std::string_view func_name,
                                     std::string_view args) {
    try {
        sol::protected_function func = (*vm->state())[std::string(func_name)];
        if (!func.valid()) {
            return R"({"error": "function not found"})";
        }

        auto result = func();
        if (result.valid()) {
            return R"({"ok": true})";
        } else {
            sol::error err = result;
            return R"({"error": ")" + std::string(err.what()) + R"("})";
        }
    } catch (const std::exception& e) {
        return R"({"error": ")" + std::string(e.what()) + R"("})";
    }
}

void LuaRuntime::register_api(std::shared_ptr<LuaVM> vm) {
    // Register all shield.* API functions
    register_full_shield_api(*vm->state(), impl_->service_manager);
}

void LuaRuntime::set_service_manager(LuaServiceManager* manager) {
    impl_->service_manager = manager;
}

std::string LuaRuntime::get_global(std::shared_ptr<LuaVM> vm,
                                 std::string_view name) {
    sol::state& lua = *vm->state();
    sol::object value = lua[name];

    if (value.is<std::string>()) {
        return value.as<std::string>();
    } else if (value.is<int>()) {
        return std::to_string(value.as<int>());
    } else if (value.is<double>()) {
        return std::to_string(value.as<double>());
    }
    return "";
}

void LuaRuntime::set_global(std::shared_ptr<LuaVM> vm,
                           std::string_view name,
                           std::string_view value) {
    sol::state& lua = *vm->state();
    lua[name] = std::string(value);
}

}  // namespace shield::lua
