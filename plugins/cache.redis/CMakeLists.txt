# cache.redis — Redis provider for shield.cache.v1 (plugin system v1).
add_library(shield_cache_redis MODULE shield_cache_redis.cpp)

target_include_directories(shield_cache_redis
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/cache.redis
)

find_package(redis++ CONFIG REQUIRED)
target_link_libraries(shield_cache_redis PRIVATE redis++::redis++)

set_target_properties(shield_cache_redis PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_cache_redis"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/cache.redis/bin"
)

file(COPY plugin.json
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/cache.redis")

message(STATUS "  + cache.redis: Redis-backed cache (v1 ABI)")
