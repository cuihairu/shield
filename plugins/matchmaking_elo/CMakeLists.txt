# matchmaking.elo — ELO provider for shield.matchmaking.v1 (plugin system v1).
#
# Builds a runtime-loadable MODULE library implementing shield_plugin_get_v1.
# Deployed to <runtime>/plugins/matchmaking.elo/{plugin.json,bin/...} so
# PluginHost's scan() finds the manifest and load() maps the library.
add_library(shield_matchmaking_elo MODULE shield_matchmaking_elo.cpp)

target_include_directories(shield_matchmaking_elo
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

# Package layout: plugins/matchmaking.elo/{plugin.json, bin/lib...}
set_target_properties(shield_matchmaking_elo PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_matchmaking_elo"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/matchmaking.elo/bin"
)

# Copy the manifest next to the binary so scan() finds it.
file(COPY plugin.json
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/matchmaking.elo")

message(STATUS "  + matchmaking.elo: ELO provider (v1 ABI)")
