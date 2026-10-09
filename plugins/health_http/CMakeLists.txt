# health.http — HTTP health provider for shield.health.v1 (plugin system v1).
#
# Builds a runtime-loadable MODULE library implementing shield_plugin_get_v1.
# Embeds a minimal boost::beast HTTP listener (does NOT link shield_net).
find_package(Boost REQUIRED COMPONENTS system)

# Object core so the plugin tests can link the plugin translation unit
# directly (same structure as protocol.json) instead of dlopen'ing the module.
add_library(shield_health_http_obj OBJECT shield_health_http.cpp)
set_target_properties(shield_health_http_obj PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(shield_health_http_obj
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${Boost_INCLUDE_DIRS}
)
target_link_libraries(shield_health_http_obj PRIVATE Boost::system)

add_library(shield_health_http MODULE
    $<TARGET_OBJECTS:shield_health_http_obj>
)

target_include_directories(shield_health_http
    PRIVATE
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
