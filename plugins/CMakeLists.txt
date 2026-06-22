# Shield plugins.
#
# Each subdirectory builds a runtime-loadable shared library implementing
# a v1 plugin C ABI (shield_plugin_get_v1). Plugins are mutually independent.

# --- Database plugins ---

if(SHIELD_BUILD_DB_PLUGIN_MYSQL)
    add_subdirectory(mysql)
endif()

if(SHIELD_BUILD_DB_PLUGIN_POSTGRESQL)
    add_subdirectory(postgresql)
endif()

if(SHIELD_BUILD_DB_PLUGIN_SQLITE)
    add_subdirectory(sqlite)
endif()

if(SHIELD_BUILD_PLUGIN_MONGODB)
    add_subdirectory(mongodb)
endif()

# --- Business plugins ---

if(SHIELD_BUILD_PLUGIN_AUTH)
    add_subdirectory(auth_jwt)
endif()

if(SHIELD_BUILD_PLUGIN_METRIC)
    add_subdirectory(metric_prometheus)
endif()

if(SHIELD_BUILD_PLUGIN_HEALTH)
    add_subdirectory(health_http)
endif()

if(SHIELD_BUILD_PLUGIN_MATCHMAKING)
    add_subdirectory(matchmaking_elo)
endif()

# --- Redis-backed infrastructure plugins ---
#
# v1 deliberately does NOT expose "redis" as a public plugin interface
# (see docs/plugin-system.md). Each of cache/queue/leaderboard connects to
# Redis independently via redis++; they share data through Redis itself,
# not through a shared plugin dependency.

if(SHIELD_BUILD_PLUGIN_CACHE_REDIS)
    add_subdirectory(cache.redis)
endif()

if(SHIELD_BUILD_PLUGIN_QUEUE_REDIS)
    add_subdirectory(queue.redis)
endif()

if(SHIELD_BUILD_PLUGIN_LEADERBOARD_REDIS)
    add_subdirectory(leaderboard.redis)
endif()
