#include "shield/script/lua_engine.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#include "shield/log/logger.hpp"

namespace shield::script {

LuaEngine::LuaEngine(const std::string& name)
    : Component(name), initialized_(false) {}

LuaEngine::~LuaEngine() {
    // sol2 handles cleanup automatically
}

void LuaEngine::on_init() {
    try {
        lua_state_.open_libraries(sol::lib::base, sol::lib::package,
                                  sol::lib::string, sol::lib::math,
                                  sol::lib::table, sol::lib::io, sol::lib::os,
                                  sol::lib::debug, sol::lib::coroutine);
        initialized_ = true;
        SHIELD_LOG_INFO << "LuaEngine initialized successfully";
    } catch (const sol::error& e) {
        SHIELD_LOG_ERROR << "Failed to initialize LuaEngine: " << e.what();
        throw std::runtime_error("Failed to initialize Lua state");
    }
}

void LuaEngine::on_stop() {
    initialized_ = false;
    // sol2 will handle cleanup automatically
    SHIELD_LOG_INFO << "LuaEngine stopped";
}

bool LuaEngine::load_script(const std::string& filename) {
    if (!initialized_) {
        SHIELD_LOG_ERROR << "LuaEngine not initialized";
        return false;
    }

    try {
        auto result = lua_state_.script_file(filename);
        if (result.valid()) {
            SHIELD_LOG_INFO << "Successfully loaded script: " << filename;
            return true;
        } else {
            sol::error err = result;
            SHIELD_LOG_ERROR << "Failed to load script " << filename << ": "
                             << err.what();
            return false;
        }
    } catch (const sol::error& e) {
        SHIELD_LOG_ERROR << "Exception loading script " << filename << ": "
                         << e.what();
        return false;
    }
}

bool LuaEngine::execute_string(const std::string& code) {
    if (!initialized_) {
        SHIELD_LOG_ERROR << "LuaEngine not initialized";
        return false;
    }

    try {
        auto result = lua_state_.script(code);
        if (result.valid()) {
            return true;
        } else {
            sol::error err = result;
            SHIELD_LOG_ERROR << "Failed to execute code: " << err.what();
            return false;
        }
    } catch (const sol::error& e) {
        SHIELD_LOG_ERROR << "Exception executing code: " << e.what();
        return false;
    }
}

}  // namespace shield::script