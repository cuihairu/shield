#pragma once

#include "shield/core/starter.hpp"

namespace shield::gateway {

/**
 * Gateway Starter - Initializes gateway networking services.
 * This Starter sets up the GatewayService for handling various network
 * protocols.
 */
class GatewayStarter : public core::IStarter {
public:
    void initialize(core::ApplicationContext& context) override;

    std::string name() const override { return "GatewayStarter"; }

    std::vector<std::string> depends_on() const override {
        // Gateway depends on Script and Actor systems
        return {"ScriptStarter", "ActorStarter"};
    }
};

}  // namespace shield::gateway