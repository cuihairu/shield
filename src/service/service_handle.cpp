#include "shield/service/service_handle.hpp"

#include <sstream>

namespace shield::service {

ServiceHandle::ServiceHandle(caf::actor handle, std::string name,
                              bool is_local)
    : handle_(std::move(handle)),
      name_(std::move(name)),
      is_local_(is_local) {}


ServiceHandle::operator bool() const { return static_cast<bool>(handle_); }

bool ServiceHandle::valid() const { return static_cast<bool>(handle_); }

bool ServiceHandle::is_local() const { return is_local_; }

const std::string& ServiceHandle::name() const { return name_; }

std::string ServiceHandle::to_string() const {
    if (!handle_) return "shield://invalid";
    std::ostringstream oss;
    oss << "shield://" << handle_->node() << "/" << handle_->id();
    if (!name_.empty()) oss << "#" << name_;
    return oss.str();
}

ServiceHandle ServiceHandle::from_string(const std::string& str) {
    // Format: shield://node/id#name
    if (str.substr(0, 10) != "shield://") return ServiceHandle();
    auto rest = str.substr(10);
    auto hash_pos = rest.find('#');
    std::string name;
    if (hash_pos != std::string::npos) {
        name = rest.substr(hash_pos + 1);
        rest = rest.substr(0, hash_pos);
    }
    // Note: full resolution requires actor_system, handled in query()
    return ServiceHandle(caf::actor{}, name, false);
}

const caf::actor& ServiceHandle::caf_handle() const { return handle_; }

ServiceHandle::operator caf::actor() const { return handle_; }

}  // namespace shield::service
