# Database backend plugins.
#
# Each subdirectory builds a runtime-loadable shared library implementing
# the C ABI declared in include/shield/data/db_plugin.h. The plugins are
# mutually independent - users can build any subset.

if(SHIELD_BUILD_DB_PLUGIN_MYSQL)
    add_subdirectory(mysql)
endif()

if(SHIELD_BUILD_DB_PLUGIN_POSTGRESQL)
    add_subdirectory(postgresql)
endif()

if(SHIELD_BUILD_DB_PLUGIN_SQLITE)
    add_subdirectory(sqlite)
endif()
