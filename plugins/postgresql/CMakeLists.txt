# database.postgresql — libpq provider for shield.database.v1 (plugin system v1).
add_library(shield_db_pgsql MODULE shield_db_pgsql.cpp)

target_include_directories(shield_db_pgsql
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/postgresql
)

find_package(PostgreSQL REQUIRED)
target_link_libraries(shield_db_pgsql PRIVATE PostgreSQL::PostgreSQL)

set_target_properties(shield_db_pgsql PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_db_pgsql"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/database.postgresql/bin"
)

file(COPY plugin.json
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/database.postgresql")

message(STATUS "  + database.postgresql: PostgreSQL (libpq) provider (v1 ABI)")
