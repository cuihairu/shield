// [SHIELD_LUA] Lua Runtime implementation
#include "shield/lua/lua_runtime.hpp"

#include <sol/sol.hpp>

// Forward declaration
namespace shield::lua {
void register_full_shield_api(sol::state& lua);
}

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

private:
    std::shared_ptr<sol::state> state_;
};

struct LuaRuntime::Impl {
    std::shared_ptr<sol::state> default_state;

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
    register_full_shield_api(*vm->state());
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
