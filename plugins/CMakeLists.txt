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
# redis.driver is the unified Redis driver that owns the redis++ connection
# pool and exposes shield.redis.v1. Other Redis plugins (cache, queue,
# leaderboard) depend on it instead of connecting to Redis independently.

if(SHIELD_BUILD_PLUGIN_REDIS_DRIVER)
    add_subdirectory(redis.driver)
endif()

if(SHIELD_BUILD_PLUGIN_CACHE_REDIS)
    add_subdirectory(cache.redis)
endif()

if(SHIELD_BUILD_PLUGIN_QUEUE_REDIS)
    add_subdirectory(queue.redis)
endif()

if(SHIELD_BUILD_PLUGIN_LEADERBOARD_REDIS)
    add_subdirectory(leaderboard.redis)
endif()

# --- Protocol codec plugins ---

if(SHIELD_BUILD_PLUGIN_PROTOBUF)
    add_subdirectory(protocol.protobuf)
endif()
