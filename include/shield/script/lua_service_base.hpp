// [CORE]
#pragma once

#include <string>

#include "shield/core/service.hpp"

namespace shield::service {
class ServiceContext;
}

namespace shield::script {

// C++ side: wraps a Lua script as a named service.
// Owns the script path, handles reload.
class LuaServiceBase : public core::Service {
public:
    LuaServiceBase(const std::string& name,
                   const std::string& script_path);

    void on_init(core::ApplicationContext& ctx) override;
    void on_start() override;
    void on_stop() override;
    std::string name() const override { return name_; }

    const std::string& script_path() const { return script_path_; }

    // Hot-reload: re-read the script file and replace on_message handlers.
    // Returns true if the file was reloaded, false if unchanged or on error.
    bool reload_script();

private:
    std::string name_;
    std::string script_path_;
};

}  // namespace shield::script
