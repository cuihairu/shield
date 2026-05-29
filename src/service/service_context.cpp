#include "shield/service/service_context.hpp"

namespace shield::service {

namespace {
thread_local ServiceContext* tl_current = nullptr;
}

ServiceContext& ServiceContext::current() {
    if (!tl_current) {
        throw std::runtime_error("No ServiceContext set in current thread");
    }
    return *tl_current;
}

bool ServiceContext::has_current() { return tl_current != nullptr; }

ServiceContext::ServiceContext(actor::DistributedActorSystem& dist_system,
                               caf::actor_system& caf_system)
    : dist_system_(&dist_system),
      caf_system_(&caf_system),
      node_id_(dist_system.get_node_id()) {}

actor::DistributedActorSystem& ServiceContext::distributed_system() {
    return *dist_system_;
}

caf::actor_system& ServiceContext::caf_system() { return *caf_system_; }

void ServiceContext::set_self(caf::event_based_actor* self) { self_ = self; }

caf::event_based_actor* ServiceContext::self() const { return self_; }

const std::string& ServiceContext::node_id() const { return node_id_; }

ServiceContext::Guard::Guard(ServiceContext& ctx) : prev_(tl_current) {
    tl_current = &ctx;
}

ServiceContext::Guard::~Guard() { tl_current = prev_; }

}  // namespace shield::service
