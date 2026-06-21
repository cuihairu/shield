add_library(shield_metric_prometheus_plugin SHARED shield_metric_prometheus.cpp)
target_include_directories(shield_metric_prometheus_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(shield_metric_prometheus_plugin PROPERTIES OUTPUT_NAME "shield_metric_prometheus" PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
