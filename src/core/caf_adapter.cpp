// [SHIELD_CORE] CAF adapter implementation
#include "shield/core/caf_adapter.hpp"
#include "shield/core/service_registry.hpp"

#include "shield/base/error.hpp"
#include "shield/base/id.hpp"
#include "shield/base/time.hpp"

#include <caf/actor_system.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/scheduled_actor/traits.hpp>
#include <caf/stateful_actor.hpp>
#include <caf/io/all.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace shield::core {

namespace {

// Next ID for timers and tasks
std::atomic<uint64_t> g_next_id{1};

// Thread-local current service handle
thread_local ServiceHandle g_current_service;

// Message types for CAF
struct ShieldMessage {
    std::string sender;
    std::string method;
    std::string payload;  // JSON
    base::TraceId trace;
    bool expects_response = false;
    caf::response_promise promise;
};

// CAF type ID for ShieldMessage
CAF_TYPE_ID(shield::core, ShieldMessage, "shield::core::ShieldMessage");

}  // namespace

struct CafAdapter::Impl {
    caf::actor_system& system;
    std::unique_ptr<ServiceRegistry> registry;

    // Timer management
    std::unordered_map<uint64_t, caf::actor> timer_actors;
    std::mutex timer_mutex;

    // Task management
    std::unordered_map<uint64_t, caf::actor> task_actors;
    std::mutex task_mutex;

    Impl(caf::actor_system& sys)
        : system(sys), registry(std::make_unique<ServiceRegistry>()) {}
};

CafAdapter::CafAdapter(caf::actor_system& system)
    : impl_(std::make_unique<Impl>(system)), system_(system) {}

CafAdapter::~CafAdapter() {
    // Clean up timers and tasks
    {
        std::lock_guard lock(impl_->timer_mutex);
        impl_->timer_actors.clear();
    }
    {
        std::lock_guard lock(impl_->task_mutex);
        impl_->task_actors.clear();
    }
}

ServiceHandle CafAdapter::spawn_service(
    std::string name,
    std::function<void(caf::event_based_actor*)> behavior) {

    caf::actor actor = system_.spawn([
        behavior = std::move(behavior),
        name
    ](caf::event_based_actor* self) -> caf::behavior {
        // Set current service
        g_current_service.internal_set(
            reinterpret_cast<detail::ActorHolder*>(self->id()));

        // Call user behavior
        behavior(self);

        // Default behavior: handle ShieldMessage
        return self->behavior().or_else(
            caf::behavior{
                [](const ShieldMessage& msg) {
                    // Handle shield messages
                    if (msg.expects_response && msg.promise) {
                        // Send response
                        msg.promise.deliver(
                            R"({"ok": true, "result": null})");
                    }
                }
            }
        );
    });

    auto holder = new detail::ActorHolder(std::move(actor));
    ServiceHandle handle(holder, std::move(name), true);

    // Register in local registry
    impl_->registry->register_service(handle.name(), handle);

    return handle;
}

void CafAdapter::send(const ServiceHandle& target,
                      const MessageEnvelope& envelope) {
    if (!target.is_valid()) return;

    auto& actor = target.internal()->get();

    // Create CAF message
    ShieldMessage msg;
    msg.method = envelope.method();
    msg.payload = std::string(envelope.payload().begin(),
                             envelope.payload().end());
    msg.expects_response = false;

    caf::anon_send(actor, std::move(msg));
}

MessageResponse CafAdapter::call(const ServiceHandle& target,
                                const MessageEnvelope& envelope) {
    if (!target.is_valid()) {
        return MessageResponse::error(
            base::Error(base::errors::INVALID_ARGUMENT,
                       "Invalid service handle"));
    }

    auto& actor = target.internal()->get();

    // This would use scoped_actor and request/response
    // For now, return a dummy response
    return MessageResponse::ok(Payload{});
}

// Timer actor
caf::behavior timer_actor_impl(caf::event_based_actor* self,
                              uint64_t timer_id,
                              int32_t delay_ms,
                              bool repeat,
                              std::function<void()> callback) {
    auto timeout = base::from_millis(delay_ms);

    if (repeat) {
        // Repeating timer
        self->delayed_send(timeout, self, atom::tick);
        return caf::behavior{
            [=](caf::atom_value) {
                callback();
                self->delayed_send(timeout, self, atom::tick);
            }
        };
    } else {
        // One-shot timer
        self->delayed_send(timeout, self, atom::tick);
        return caf::behavior{
            [=](caf::atom_value) {
                callback();
                self->quit();
            }
        };
    }
}

TimerHandle CafAdapter::timeout_once(int32_t delay_ms,
                                    std::function<void()> callback) {
    uint64_t id = g_next_id.fetch_add(1);

    auto actor = system_.spawn(timer_actor_impl, id, delay_ms, false,
                              std::move(callback));

    std::lock_guard lock(impl_->timer_mutex);
    impl_->timer_actors[id] = std::move(actor);

    TimerHandle handle;
    handle.id_ = id;
    return handle;
}

TimerHandle CafAdapter::timer_repeat(int32_t interval_ms,
                                    std::function<void()> callback) {
    uint64_t id = g_next_id.fetch_add(1);

    auto actor = system_.spawn(timer_actor_impl, id, interval_ms, true,
                              std::move(callback));

    std::lock_guard lock(impl_->timer_mutex);
    impl_->timer_actors[id] = std::move(actor);

    TimerHandle handle;
    handle.id_ = id;
    return handle;
}

void CafAdapter::cancel_timer(const TimerHandle& timer) {
    if (!timer.is_valid()) return;

    std::lock_guard lock(impl_->timer_mutex);
    auto it = impl_->timer_actors.find(timer.id_);
    if (it != impl_->timer_actors.end()) {
        caf::anon_send_exit(it->second, caf::exit_reason::user_shutdown);
        impl_->timer_actors.erase(it);
    }
}

// Task actor
caf::behavior task_actor_impl(caf::event_based_actor* self,
                             uint64_t task_id,
                             std::function<void()> task) {
    task();
    self->quit();
    return caf::behavior{};
}

TaskHandle CafAdapter::fork(std::function<void()> task) {
    uint64_t id = g_next_id.fetch_add(1);

    auto actor = system_.spawn(task_actor_impl, id, std::move(task));

    std::lock_guard lock(impl_->task_mutex);
    impl_->task_actors[id] = std::move(actor);

    TaskHandle handle;
    handle.id_ = id;
    return handle;
}

ServiceHandle CafAdapter::current_service() {
    return g_current_service;
}

// Registry access
ServiceRegistry& CafAdapter::registry() {
    return *impl_->registry;
}

// Initialize core function
std::unique_ptr<CafAdapter> initialize_core(caf::actor_system& system) {
    return std::make_unique<CafAdapter>(system);
}

}  // namespace shield::core
