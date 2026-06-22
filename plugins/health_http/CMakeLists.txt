# health.http — HTTP health provider for shield.health.v1 (plugin system v1).
#
# Builds a runtime-loadable MODULE library implementing shield_plugin_get_v1.
# Embeds a minimal boost::beast HTTP listener (does NOT link shield_net).
find_package(Boost REQUIRED COMPONENTS system)

add_library(shield_health_http MODULE shield_health_http.cpp)

target_include_directories(shield_health_http
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${Boost_INCLUDE_DIRS}
)

target_link_libraries(shield_health_http PRIVATE Boost::system)

set_target_properties(shield_health_http PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_health_http"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/health.http/bin"
)

file(COPY manifest.yaml
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/health.http")

message(STATUS "  + health.http: HTTP health endpoint (v1 ABI, boost::beast)")
