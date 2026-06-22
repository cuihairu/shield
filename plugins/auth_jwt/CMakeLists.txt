# auth.jwt — HS256 JWT provider for shield.auth.v1 (plugin system v1).
#
# Builds a runtime-loadable MODULE library implementing shield_plugin_get_v1.
# Uses OpenSSL's HMAC-SHA256 for token signing / verification.
add_library(shield_auth_jwt MODULE shield_auth_jwt.cpp)

target_include_directories(shield_auth_jwt
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

find_package(OpenSSL REQUIRED)
target_link_libraries(shield_auth_jwt PRIVATE OpenSSL::Crypto)

set_target_properties(shield_auth_jwt PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_auth_jwt"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/auth.jwt/bin"
)

file(COPY manifest.yaml
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/auth.jwt")

message(STATUS "  + auth.jwt: HS256 JWT provider (v1 ABI, OpenSSL)")
