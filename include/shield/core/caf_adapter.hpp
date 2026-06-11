// [SHIELD_CORE] CAF adapter - internal bridge to CAF
#pragma once

// Note: This header is INTERNAL to shield_core implementation
// Public headers should NOT include this

#include "shield/core_new/service_handle.hpp"
#include "shield/core_new/message.hpp"

#include <caf/actor.hpp>
#include <caf/actor_system.hpp>
#include <caf/event_based_actor.hpp>

#include <memory>
#include <string>

namespace shield::core::detail {

/// @brief Internal holder for CAF actor
/// Opaque wrapper around caf::actor
class ActorHolder {
public:
    ActorHolder();
    explicit ActorHolder(caf::actor actor);

    bool is_valid() const { return static_cast<bool>(actor_); }
    const caf::actor& get() const { return actor_; }

private:
    caf::actor actor_;
};

}  // namespace shield::core::detail

namespace shield::core {

class ServiceRegistry;
class ServiceContext;

/// @brief CAF adapter - bridges Shield API to CAF
/// This class is the ONLY place in shield_core that knows about CAF
class CafAdapter {
public:
    CafAdapter(caf::actor_system& system);
    ~CafAdapter();

    // Non-copyable, non-movable
    CafAdapter(const CafAdapter&) = delete;
    CafAdapter& operator=(const CafAdapter&) = delete;

    // Get the underlying actor system
    caf::actor_system& system() { return system_; }

    // Service spawning
    ServiceHandle spawn_service(
        std::string name,
        std::function<void(caf::event_based_actor*)> behavior);

    // Message sending (fire-and-forget)
    void send(const ServiceHandle& target, const MessageEnvelope& envelope);

    // Message calling (request-response)
    MessageResponse call(const ServiceHandle& target,
                       const MessageEnvelope& envelope);

    // Timer operations
    TimerHandle timeout_once(int32_t delay_ms,
                             std::function<void()> callback);

    TimerHandle timer_repeat(int32_t interval_ms,
                             std::function<void()> callback);

    void cancel_timer(const TimerHandle& timer);

    // Task operations (fork)
    TaskHandle fork(std::function<void()> task);

    // Get current service handle (if called from within a service)
    ServiceHandle current_service();

    // Get service registry
    ServiceRegistry& registry();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    caf::actor_system& system_;
};

// CAF atoms
namespace atom {
inline constexpr auto tick = caf::atom("tick");
}

// Initialize core with actor system
std::unique_ptr<CafAdapter> initialize_core(caf::actor_system& system);

}  // namespace shield::core
