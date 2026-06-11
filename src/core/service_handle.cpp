// [SHIELD_CORE] Service handle implementation
#include "shield/core_new/service_handle.hpp"
#include "shield/core_new/caf_adapter.hpp"

#include <caf/actor.hpp>

namespace shield::core {

namespace detail {

class ActorHolder {
public:
    ActorHolder() = default;

    explicit ActorHolder(caf::actor actor)
        : actor_(std::move(actor)) {}

    bool is_valid() const {
        return static_cast<bool>(actor_);
    }

    const caf::actor& get() const {
        return actor_;
    }

    caf::actor& get() {
        return actor_;
    }

private:
    caf::actor actor_;
};

}  // namespace detail

// ServiceHandle implementation
ServiceHandle::ServiceHandle() = default;

ServiceHandle::ServiceHandle(detail::ActorHolder* holder, std::string name,
                             bool is_local)
    : holder_(holder), name_(std::move(name)), is_local_(is_local) {}

ServiceHandle::~ServiceHandle() {
    // Don't delete holder_ - it's managed by CAF
}

ServiceHandle::ServiceHandle(const ServiceHandle& other)
    : holder_(other.holder_), name_(other.name_),
      is_local_(other.is_local_) {}

ServiceHandle::ServiceHandle(ServiceHandle&& other) noexcept
    : holder_(other.holder_), name_(std::move(other.name_)),
      is_local_(other.is_local_) {
    other.holder_ = nullptr;
}

ServiceHandle& ServiceHandle::operator=(const ServiceHandle& other) {
    if (this != &other) {
        holder_ = other.holder_;
        name_ = other.name_;
        is_local_ = other.is_local_;
    }
    return *this;
}

ServiceHandle& ServiceHandle::operator=(ServiceHandle&& other) noexcept {
    if (this != &other) {
        holder_ = other.holder_;
        name_ = std::move(other.name_);
        is_local_ = other.is_local_;
        other.holder_ = nullptr;
    }
    return *this;
}

bool ServiceHandle::is_valid() const {
    return holder_ && holder_->is_valid();
}

std::string ServiceHandle::to_string() const {
    if (!is_valid()) return "shield://invalid";
    return "shield://local/" + name_;
}

void ServiceHandle::internal_set(detail::ActorHolder* holder) {
    holder_ = holder;
}

// TimerHandle implementation
TimerHandle::TimerHandle() : id_(0) {}

TimerHandle::~TimerHandle() = default;

TimerHandle::TimerHandle(const TimerHandle& other) : id_(other.id_) {}

TimerHandle::TimerHandle(TimerHandle&& other) noexcept
    : id_(other.id_) {
    other.id_ = 0;
}

TimerHandle& TimerHandle::operator=(const TimerHandle& other) {
    if (this != &other) {
        id_ = other.id_;
    }
    return *this;
}

TimerHandle& TimerHandle::operator=(TimerHandle&& other) noexcept {
    if (this != &other) {
        id_ = other.id_;
        other.id_ = 0;
    }
    return *this;
}

bool TimerHandle::is_valid() const {
    return id_ != 0;
}

void TimerHandle::cancel() {
    id_ = 0;
}

// TaskHandle implementation
TaskHandle::TaskHandle() : id_(0) {}

TaskHandle::~TaskHandle() = default;

TaskHandle::TaskHandle(const TaskHandle& other) : id_(other.id_) {}

TaskHandle::TaskHandle(TaskHandle&& other) noexcept
    : id_(other.id_) {
    other.id_ = 0;
}

TaskHandle& TaskHandle::operator=(const TaskHandle& other) {
    if (this != &other) {
        id_ = other.id_;
    }
    return *this;
}

TaskHandle& TaskHandle::operator=(TaskHandle&& other) noexcept {
    if (this != &other) {
        id_ = other.id_;
        other.id_ = 0;
    }
    return *this;
}

bool TaskHandle::is_valid() const {
    return id_ != 0;
}

void TaskHandle::cancel() {
    id_ = 0;
}

}  // namespace shield::core
