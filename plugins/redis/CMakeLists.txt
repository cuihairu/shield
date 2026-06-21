# Redis-based plugins: REDIS (infrastructure), CACHE, QUEUE, LEADERBOARD.
# CACHE/QUEUE/LEADERBOARD depend on REDIS for storage.

# Redis infrastructure plugin
add_library(shield_redis_plugin SHARED shield_redis.cpp)
target_include_directories(shield_redis_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(shield_redis_plugin PRIVATE redis++::redis++ hiredis::hiredis)
set_target_properties(shield_redis_plugin PROPERTIES OUTPUT_NAME "shield_redis" PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Cache plugin
add_library(shield_cache_plugin SHARED shield_cache_redis.cpp)
target_include_directories(shield_cache_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(shield_cache_plugin PRIVATE redis++::redis++ hiredis::hiredis)
set_target_properties(shield_cache_plugin PROPERTIES OUTPUT_NAME "shield_cache" PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Queue plugin
add_library(shield_queue_plugin SHARED shield_queue_redis.cpp)
target_include_directories(shield_queue_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(shield_queue_plugin PRIVATE redis++::redis++ hiredis::hiredis)
set_target_properties(shield_queue_plugin PROPERTIES OUTPUT_NAME "shield_queue" PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Leaderboard plugin
add_library(shield_leaderboard_plugin SHARED shield_leaderboard_redis.cpp)
target_include_directories(shield_leaderboard_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(shield_leaderboard_plugin PRIVATE redis++::redis++ hiredis::hiredis)
set_target_properties(shield_leaderboard_plugin PROPERTIES OUTPUT_NAME "shield_leaderboard" PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
