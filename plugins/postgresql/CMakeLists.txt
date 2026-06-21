# PostgreSQL plugin for shield_data.
#
# Wraps libpq (PostgreSQL's C client) behind the shield_db_plugin ABI.
# libpq is a thin layer over the wire protocol, so the resulting plugin
# is small. Heavier deps (Kerberos, OpenSSL, etc.) live inside libpq.dll
# itself rather than being pulled in transitively.
add_library(shield_db_pgsql SHARED
    shield_db_pgsql.cpp
)

target_include_directories(shield_db_pgsql
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/postgresql
)

find_package(PostgreSQL REQUIRED)  # ships libpq on all platforms via vcpkg
target_link_libraries(shield_db_pgsql PRIVATE PostgreSQL::PostgreSQL)

set_target_properties(shield_db_pgsql PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
)

message(STATUS "  + shield_db_pgsql: PostgreSQL (libpq) backend plugin")
