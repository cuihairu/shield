# Redis plugin: shared infrastructure for CACHE/QUEUE/LEADERBOARD.
# Wraps redis++ into the shield_redis_plugin C ABI interface.

add_library(shield_redis_plugin SHARED
    shield_redis.cpp
)

target_include_directories(shield_redis_plugin
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

target_link_libraries(shield_redis_plugin
    PRIVATE redis++::redis++ hiredis::hiredis
)

# Install next to the host executable.
set_target_properties(shield_redis_plugin PROPERTIES
    OUTPUT_NAME "shield_redis"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
