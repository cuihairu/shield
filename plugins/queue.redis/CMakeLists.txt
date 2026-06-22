# queue.redis — Redis pub/sub provider for shield.queue.v1 (plugin system v1).
#
# This plugin owns its Lua autonomy: register_lua() builds the callable
# shield.queue.redis namespace from this DLL, not from the host. sol2 and
# Lua are therefore PRIVATE dependencies — their symbols must NOT leak into
# the host process (sol2 is header-only but needs Lua's headers at compile
# time and lua52/53/luajit import library at link time).
add_library(shield_queue_redis MODULE shield_queue_redis.cpp)

target_include_directories(shield_queue_redis
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/queue.redis
        ${LUA_INCLUDE_DIR}
        ${SOL2_INCLUDE_DIRS}
)

find_package(redis++ CONFIG REQUIRED)
target_link_libraries(shield_queue_redis
    PRIVATE
        redis++::redis++
        sol2::sol2
        ${LUA_LIBRARIES}
)

set_target_properties(shield_queue_redis PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_queue_redis"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/queue.redis/bin"
)

file(COPY plugin.json
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/queue.redis")

message(STATUS "  + queue.redis: Redis pub/sub queue (v1 ABI, Lua-autonomous)")
