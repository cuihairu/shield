// [SHIELD_CORE] Main public header for shield_core
#pragma once

// Public types
#include "shield/core/service_handle.hpp"
#include "shield/core/message.hpp"
#include "shield/core/service_registry.hpp"

#include <memory>

// Forward declaration
namespace shield::core {

class CafAdapter;
namespace caf_detail {
class ActorSystemHolder;
}

}  // namespace shield::core
