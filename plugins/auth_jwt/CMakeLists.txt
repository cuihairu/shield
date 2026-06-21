add_library(shield_auth_jwt_plugin SHARED shield_auth_jwt.cpp)
target_include_directories(shield_auth_jwt_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(shield_auth_jwt_plugin PROPERTIES OUTPUT_NAME "shield_auth_jwt" PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
