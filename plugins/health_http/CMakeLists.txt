add_library(shield_health_http_plugin SHARED shield_health_http.cpp)
target_include_directories(shield_health_http_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(shield_health_http_plugin PROPERTIES OUTPUT_NAME "shield_health_http" PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
