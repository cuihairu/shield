# cache.redis — Redis provider for shield.cache.v1 (plugin system v1).
#
# Builds a runtime-loadable MODULE library implementing shield_plugin_get_v1.
# Deployed to <runtime>/plugins/cache.redis/{plugin.json,bin/...} so
# PluginHost's scan() finds the manifest and load() maps the library.
#
# This plugin owns its Lua autonomy: register_lua() builds the callable
# shield.cache.redis namespace from this DLL, not from the host. sol2 and
# Lua are therefore PRIVATE dependencies — their symbols must NOT leak into
# the host process (sol2 is header-only but needs Lua's headers at compile
# time and lua52/53/luajit import library at link time).
add_library(shield_cache_redis MODULE shield_cache_redis.cpp)

target_include_directories(shield_cache_redis
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/cache.redis
        ${LUA_INCLUDE_DIR}
        ${SOL2_INCLUDE_DIRS}
)

find_package(redis++ CONFIG REQUIRED)
target_link_libraries(shield_cache_redis
    PRIVATE
        redis++::redis++
        sol2::sol2
        ${LUA_LIBRARIES}
)

set_target_properties(shield_cache_redis PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_cache_redis"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/cache.redis/bin"
)

file(COPY plugin.json
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/cache.redis")

message(STATUS "  + cache.redis: Redis-backed cache (v1 ABI, Lua-autonomous)")
