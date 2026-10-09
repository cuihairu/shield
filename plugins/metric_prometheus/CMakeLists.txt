# metrics.prometheus — Prometheus exporter for shield.metrics.v1 (plugin system v1).
find_package(Boost REQUIRED COMPONENTS system)

# Object core so the plugin tests can link the plugin translation unit
# directly (same structure as protocol.json) instead of dlopen'ing the module.
add_library(shield_metric_prometheus_obj OBJECT shield_metric_prometheus.cpp)
set_target_properties(shield_metric_prometheus_obj PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(shield_metric_prometheus_obj
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${Boost_INCLUDE_DIRS}
)
target_link_libraries(shield_metric_prometheus_obj PRIVATE Boost::system)

add_library(shield_metric_prometheus MODULE
    $<TARGET_OBJECTS:shield_metric_prometheus_obj>
)

target_include_directories(shield_metric_prometheus
    PRIVATE
        ${Boost_INCLUDE_DIRS}
)

target_link_libraries(shield_metric_prometheus PRIVATE Boost::system)

set_target_properties(shield_metric_prometheus PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_metric_prometheus"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/metrics.prometheus/bin"
)

file(COPY manifest.yaml
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/metrics.prometheus")

message(STATUS "  + metrics.prometheus: Prometheus text exporter (v1 ABI, boost::beast)")
