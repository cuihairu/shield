// [SHIELD_LUA] Lua runtime constants
#pragma once

#include <cstddef>
#include <cstdint>

namespace shield::lua {

// ============================================================================
// Resource Limits
// ============================================================================

/// @brief Maximum number of active timers per service
static constexpr size_t kTimerLimit = 10000;

/// @brief Maximum number of pending forked tasks per service
static constexpr size_t kForkLimit = 1000;

/// @brief Maximum size of a single message in bytes (1 MB)
static constexpr size_t kMaxMessageSize = 1024 * 1024;

// ============================================================================
// Error Handling
// ============================================================================

/// @brief Number of consecutive errors before triggering panic and service exit
static constexpr int kDefaultMaxErrorsBeforePanic = 10;

// ============================================================================
// Session Management
// ============================================================================

/// @brief Interval (in resolve calls) for cleaning up expired session handles
static constexpr uint64_t kSessionCleanupInterval = 100;

// ============================================================================
// Timeouts (milliseconds)
// ============================================================================

/// @brief Default spawn timeout for on_init
static constexpr int64_t kDefaultSpawnTimeoutMs = 10000;

/// @brief Default call timeout for synchronous calls
static constexpr int32_t kDefaultCallTimeoutMs = 5000;

// ============================================================================
// Cache Configuration
// ============================================================================

/// @brief Default maximum number of cached scripts
static constexpr size_t kDefaultCacheMaxSize = 100;

/// @brief Default cache TTL in seconds (0 = no expiry)
static constexpr int64_t kDefaultCacheTtlSeconds = 0;

// ============================================================================
// CAF Actor System
// ============================================================================

/// @brief Default mail cache size for message stashing during init
static constexpr size_t kDefaultMailCacheSize = 4096;

}  // namespace shield::lua
