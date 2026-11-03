# redis.driver — Unified Redis driver for shield.redis.v2 (plugin system v1).
#
# Builds a runtime-loadable MODULE library implementing shield_plugin_get_v1.
# This plugin owns the redis++ connection pool and exposes typed Redis commands
# through the shield.redis.v2 C ABI. Other Redis plugins (cache.redis,
# queue.redis, leaderboard.redis) depend on it instead of connecting to Redis
# independently.
find_package(redis++ CONFIG REQUIRED)
# vcpkg static-only redis++ exports redis++::redis++_static; shared builds
# export redis++::redis++. Pick whichever exists.
if(TARGET redis++::redis++)
    set(SHIELD_REDISPP_TARGET redis++::redis++)
else()
    set(SHIELD_REDISPP_TARGET redis++::redis++_static)
endif()

# Object core so plugin tests can link the driver translation unit directly
# (same structure as protocol.json / health.http) instead of dlopen'ing the
# module. This makes allocator instrumentation usable for memory-contract
# tests because __wrap_malloc/free resolve within the same linked binary.
add_library(shield_redis_driver_obj OBJECT shield_redis_driver.cpp)
set_target_properties(shield_redis_driver_obj PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(shield_redis_driver_obj
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/_shared
        ${PROJECT_SOURCE_DIR}/plugins/redis.driver
        ${LUA_INCLUDE_DIR}
)

target_link_libraries(shield_redis_driver_obj
    PRIVATE
        ${SHIELD_REDISPP_TARGET}
        ${LUA_LIBRARIES}
)

add_library(shield_redis_driver MODULE
    $<TARGET_OBJECTS:shield_redis_driver_obj>
)

target_include_directories(shield_redis_driver
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/_shared
        ${PROJECT_SOURCE_DIR}/plugins/redis.driver
        ${LUA_INCLUDE_DIR}
)

target_link_libraries(shield_redis_driver
    PRIVATE
        ${SHIELD_REDISPP_TARGET}
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
