# Shield plugins.
#
# Each subdirectory builds a runtime-loadable shared library implementing
# a plugin C ABI interface. The plugins are mutually independent.

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

# --- Infrastructure plugins ---

if(SHIELD_BUILD_PLUGIN_REDIS)
    add_subdirectory(redis)
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
