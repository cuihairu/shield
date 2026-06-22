# metrics.prometheus — Prometheus exporter for shield.metrics.v1 (plugin system v1).
find_package(Boost REQUIRED COMPONENTS system)

add_library(shield_metric_prometheus MODULE shield_metric_prometheus.cpp)

target_include_directories(shield_metric_prometheus
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
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
