# queue.redis — Redis pub/sub provider for shield.queue.v1 (plugin system v1).
#
# This plugin owns its Lua autonomy: register_lua() builds the callable
# shield.queue.redis namespace from this DLL, not from the host. The Lua
# bindings go through the header-only shd layer (shield/lua/binding.hpp) and
# Lua stays a PRIVATE dependency — its symbols must NOT leak into the host
# process (headers at compile time, lua52/53/luajit import library at link
# time).
add_library(shield_queue_redis MODULE shield_queue_redis.cpp)

target_include_directories(shield_queue_redis
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/_shared
        ${PROJECT_SOURCE_DIR}/plugins/queue.redis
        ${LUA_INCLUDE_DIR}
)

find_package(redis++ CONFIG REQUIRED)
# vcpkg static-only redis++ exports redis++::redis++_static; shared builds
# export redis++::redis++. Pick whichever exists.
if(TARGET redis++::redis++)
    set(SHIELD_REDISPP_TARGET redis++::redis++)
else()
    set(SHIELD_REDISPP_TARGET redis++::redis++_static)
endif()
target_link_libraries(shield_queue_redis
    PRIVATE
        ${SHIELD_REDISPP_TARGET}
        ${LUA_LIBRARIES}
)

set_target_properties(shield_queue_redis PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_queue_redis"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/queue.redis/bin"
)

file(COPY manifest.yaml
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/queue.redis")

message(STATUS "  + queue.redis: Redis pub/sub queue (v1 ABI, Lua-autonomous)")
