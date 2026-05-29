// [CORE]
#pragma once

#include <string>

#include "caf/actor.hpp"

namespace shield::service {

class ServiceHandle {
public:
    ServiceHandle() = default;
    explicit ServiceHandle(caf::actor handle, std::string name = "",
                          bool is_local = true);

    explicit operator bool() const;
    bool valid() const;
    bool is_local() const;
    const std::string& name() const;

    std::string to_string() const;
    static ServiceHandle from_string(const std::string& str);

    const caf::actor& caf_handle() const;
    operator caf::actor() const;

private:
    caf::actor handle_;
    std::string name_;
    bool is_local_ = true;
};

}  // namespace shield::service
