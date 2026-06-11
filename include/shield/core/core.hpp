// [SHIELD_CORE] Main public header for shield_core
#pragma once

// Public types
#include "shield/core_new/service_handle.hpp"
#include "shield/core_new/message.hpp"
#include "shield/core_new/service_registry.hpp"

// Forward declaration
namespace shield::core {

class CafAdapter;

/// @brief Initialize the Shield core runtime
/// @return Unique pointer to the CAF adapter (owned by caller)
std::unique_ptr<CafAdapter> initialize_core();

}  // namespace shield::core
