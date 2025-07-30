#pragma once

#include "shield/core/starter.hpp"

namespace shield::script {

/**
 * Script Starter - Initializes Lua scripting capabilities.
 * This Starter sets up the LuaVMPool and related scripting services.
 */
class ScriptStarter : public core::IStarter {
public:
    void initialize(core::ApplicationContext& context) override;

    std::string name() const override { return "ScriptStarter"; }

    std::vector<std::string> depends_on() const override {
        // Script system has no dependencies
        return {};
    }
};

}  // namespace shield::script