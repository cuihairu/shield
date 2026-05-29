// [CORE]
#pragma once

#include <string>

#include "caf/actor_system.hpp"
#include "caf/event_based_actor.hpp"
#include "shield/actor/distributed_actor_system.hpp"

namespace shield::service {

class ServiceContext {
public:
    static ServiceContext& current();
    static bool has_current();

    ServiceContext(actor::DistributedActorSystem& dist_system,
                   caf::actor_system& caf_system);

    actor::DistributedActorSystem& distributed_system();
    caf::actor_system& caf_system();

    void set_self(caf::event_based_actor* self);
    caf::event_based_actor* self() const;

    const std::string& node_id() const;

    class Guard {
    public:
        explicit Guard(ServiceContext& ctx);
        ~Guard();
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        ServiceContext* prev_;
    };

private:
    actor::DistributedActorSystem* dist_system_;
    caf::actor_system* caf_system_;
    caf::event_based_actor* self_ = nullptr;
    std::string node_id_;
};

}  // namespace shield::service
