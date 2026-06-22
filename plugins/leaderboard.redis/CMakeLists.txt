# leaderboard.redis — Redis ZSET provider for shield.leaderboard.v1 (plugin system v1).
add_library(shield_leaderboard_redis MODULE shield_leaderboard_redis.cpp)

target_include_directories(shield_leaderboard_redis
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/leaderboard.redis
)

find_package(redis++ CONFIG REQUIRED)
target_link_libraries(shield_leaderboard_redis PRIVATE redis++::redis++)

set_target_properties(shield_leaderboard_redis PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_leaderboard_redis"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/leaderboard.redis/bin"
)

file(COPY plugin.json
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/leaderboard.redis")

message(STATUS "  + leaderboard.redis: Redis ZSET leaderboard (v1 ABI)")
