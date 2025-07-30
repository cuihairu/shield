#pragma once

#include "shield/core/starter.hpp"

namespace shield::actor {

/**
 * Actor Starter - Initializes the distributed actor system.
 * This Starter sets up the CAF actor system and distributed actor management.
 */
class ActorStarter : public core::IStarter {
public:
    void initialize(core::ApplicationContext& context) override;

    std::string name() const override { return "ActorStarter"; }

    std::vector<std::string> depends_on() const override {
        // Actor system has no dependencies
        return {};
    }
};

}  // namespace shield::actor