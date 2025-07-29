#include "shield/core/component.hpp"
#include <stdexcept>


namespace shield::core {

Component::Component(const std::string& name)
    : name_(name)
    , state_(ComponentState::CREATED) {
}

void Component::init() {
    if (state_ != ComponentState::CREATED) {
        throw std::runtime_error("Component can only be initialized in CREATED state");
    }
    on_init();
    state_ = ComponentState::INITIALIZED;
}

void Component::start() {
    if (state_ != ComponentState::INITIALIZED) {
        throw std::runtime_error("Component can only be started in INITIALIZED state");
    }
    on_start();
    state_ = ComponentState::STARTED;
}

void Component::stop() {
    if (state_ != ComponentState::STARTED) {
        throw std::runtime_error("Component can only be stopped in STARTED state");
    }
    on_stop();
    state_ = ComponentState::STOPPED;
}

void ComponentContainer::init_all() {
    for (auto& [_, component] : components_) {
        component->init();
    }
}

void ComponentContainer::start_all() {
    for (auto& [_, component] : components_) {
        component->start();
    }
}

void ComponentContainer::stop_all() {
    for (auto& [_, component] : components_) {
        component->stop();
    }
}
}
