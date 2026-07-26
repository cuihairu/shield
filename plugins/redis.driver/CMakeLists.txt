# redis.driver — Unified Redis driver for shield.redis.v1 (plugin system v1).
#
# Builds a runtime-loadable MODULE library implementing shield_plugin_get_v1.
# This plugin owns the redis++ connection pool and exposes typed Redis commands
# through the shield.redis.v1 C ABI. Other Redis plugins (cache.redis,
# queue.redis, leaderboard.redis) depend on it instead of connecting to Redis
# independently.
add_library(shield_redis_driver MODULE shield_redis_driver.cpp)

target_include_directories(shield_redis_driver
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/redis.driver
        ${LUA_INCLUDE_DIR}
        ${SOL2_INCLUDE_DIRS}
)

find_package(redis++ CONFIG REQUIRED)
# vcpkg static-only redis++ exports redis++::redis++_static; shared builds
# export redis++::redis++. Pick whichever exists.
if(TARGET redis++::redis++)
    set(SHIELD_REDISPP_TARGET redis++::redis++)
else()
    set(SHIELD_REDISPP_TARGET redis++::redis++_static)
endif()
target_link_libraries(shield_redis_driver
    PRIVATE
        ${SHIELD_REDISPP_TARGET}
        sol2::sol2
        ${LUA_LIBRARIES}
)

set_target_properties(shield_redis_driver PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_redis_driver"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/redis.driver/bin"
)

file(COPY manifest.yaml
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/redis.driver")

message(STATUS "  + redis.driver: Unified Redis driver (v1 ABI, Lua-autonomous)")
