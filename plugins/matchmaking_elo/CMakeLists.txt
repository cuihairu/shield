add_library(shield_matchmaking_elo_plugin SHARED shield_matchmaking_elo.cpp)
target_include_directories(shield_matchmaking_elo_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(shield_matchmaking_elo_plugin PROPERTIES OUTPUT_NAME "shield_matchmaking_elo" PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
