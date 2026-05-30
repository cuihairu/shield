// [CORE]
#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <vector>

#include "caf/disposable.hpp"
#include "shield/actor/actor_registry.hpp"
#include "shield/service/service_handle.hpp"

namespace shield::service {

// --- Messaging ---

void send(const ServiceHandle& target, const std::string& type,
          const std::string& payload);

void send(const std::string& target_name, const std::string& type,
          const std::string& payload);

std::future<std::string> call(
    const ServiceHandle& target, const std::string& type,
    const std::string& payload,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

std::future<std::string> call(
    const std::string& target_name, const std::string& type,
    const std::string& payload,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

// --- Timers ---

caf::disposable timeout(std::chrono::milliseconds ms,
                        std::function<void()> callback);

inline caf::disposable timeout(uint32_t ms,
                                std::function<void()> callback) {
    return timeout(std::chrono::milliseconds(ms), std::move(callback));
}

void cancel_timeout(caf::disposable& handle);

// --- Service naming/lookup ---

void name(const ServiceHandle& handle, const std::string& service_name);

ServiceHandle query(const std::string& service_name);

ServiceHandle uniqueservice(const std::string& service_name,
                            actor::ActorType type = actor::ActorType::CUSTOM);

// --- Fork ---

ServiceHandle fork(std::function<void(caf::event_based_actor*)> func,
                    const std::string& fork_name = "");

// --- Info ---

std::vector<std::string> list_services();

std::string service_info(const std::string& service_name);

std::string self_node_id();

}  // namespace shield::service
