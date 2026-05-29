#include "shield/script/lua_service_base.hpp"

#include <filesystem>

#include "shield/log/logger.hpp"

namespace shield::script {

LuaServiceBase::LuaServiceBase(const std::string& name,
                               const std::string& script_path)
    : name_(name), script_path_(script_path) {}

void LuaServiceBase::on_init(core::ApplicationContext& ctx) {
    SHIELD_LOG_INFO << "LuaServiceBase '" << name_ << "' initialized";
}

void LuaServiceBase::on_start() {
    SHIELD_LOG_INFO << "LuaServiceBase '" << name_
                    << "' started with script: " << script_path_;
}

void LuaServiceBase::on_stop() {
    SHIELD_LOG_INFO << "LuaServiceBase '" << name_ << "' stopped";
}

bool LuaServiceBase::reload_script() {
    if (!std::filesystem::exists(script_path_)) {
        SHIELD_LOG_ERROR << "LuaServiceBase::reload_script: file not found: "
                         << script_path_;
        return false;
    }

    // The actual reload is driven by sending a "reload_script" message
    // to the LuaActor owning this script. LuaServiceBase itself does not
    // hold a VM — LuaActor does. This method exists as a coordination
    // point for the hot-reload orchestrator.
    SHIELD_LOG_INFO << "LuaServiceBase::reload_script: " << script_path_;
    return true;
}

}  // namespace shield::script
