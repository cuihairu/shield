# queue.redis — Redis pub/sub provider for shield.queue.v1 (plugin system v1).
add_library(shield_queue_redis MODULE shield_queue_redis.cpp)

target_include_directories(shield_queue_redis
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/queue.redis
)

find_package(redis++ CONFIG REQUIRED)
target_link_libraries(shield_queue_redis PRIVATE redis++::redis++)

set_target_properties(shield_queue_redis PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_queue_redis"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/queue.redis/bin"
)

file(COPY plugin.json
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/queue.redis")

message(STATUS "  + queue.redis: Redis pub/sub queue (v1 ABI)")
