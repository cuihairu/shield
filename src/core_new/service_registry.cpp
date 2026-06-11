// [SHIELD_CORE] Service registry implementation
#include "shield/core_new/service_registry.hpp"
#include "shield/core_new/service_handle.hpp"

#include <shared_mutex>

namespace shield::core {

struct ServiceRegistry::Impl {
    std::unordered_map<std::string, ServiceHandle> services;
    mutable std::shared_mutex mutex;
};

ServiceRegistry::ServiceRegistry() : impl_(std::make_unique<Impl>()) {}

ServiceRegistry::~ServiceRegistry() = default;

bool ServiceRegistry::register_service(std::string_view service_name,
                                       ServiceHandle handle) {
    std::unique_lock lock(impl_->mutex);

    std::string name(service_name);
    auto [it, inserted] = impl_->services.emplace(std::move(name),
                                                   std::move(handle));
    return inserted;
}

bool ServiceRegistry::unregister_service(std::string_view service_name) {
    std::unique_lock lock(impl_->mutex);

    auto it = impl_->services.find(std::string(service_name));
    if (it == impl_->services.end()) {
        return false;
    }

    impl_->services.erase(it);
    return true;
}

ServiceHandle ServiceRegistry::find_service(std::string_view service_name) const {
    std::shared_lock lock(impl_->mutex);

    auto it = impl_->services.find(std::string(service_name));
    if (it == impl_->services.end()) {
        return ServiceHandle();
    }

    return it->second;
}

std::vector<std::string> ServiceRegistry::list_services() const {
    std::shared_lock lock(impl_->mutex);

    std::vector<std::string> result;
    result.reserve(impl_->services.size());

    for (const auto& [name, _] : impl_->services) {
        result.push_back(name);
    }

    return result;
}

bool ServiceRegistry::has_service(std::string_view service_name) const {
    std::shared_lock lock(impl_->mutex);

    return impl_->services.find(std::string(service_name)) !=
           impl_->services.end();
}

size_t ServiceRegistry::size() const {
    std::shared_lock lock(impl_->mutex);
    return impl_->services.size();
}

}  // namespace shield::core
