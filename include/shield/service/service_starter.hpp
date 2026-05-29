// [CORE]
#pragma once

#include "shield/core/starter.hpp"

namespace shield::service {

class ServiceStarter : public core::IStarter {
public:
    void initialize(core::ApplicationContext& context) override;
    std::string name() const override { return "ServiceStarter"; }
    std::vector<std::string> depends_on() const override {
        return {"ActorStarter"};
    }
};

}  // namespace shield::service
