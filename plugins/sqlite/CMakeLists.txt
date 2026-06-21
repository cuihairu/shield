# SQLite plugin for shield_data.
#
# Build into a runtime-loadable DLL so the host shield_data library never
# links sqlite3. Drop the resulting shield_db_sqlite.dll next to the host
# executable and shield::data::DatabasePool will pick it up via
# database.driver=sqlite.
add_library(shield_db_sqlite SHARED
    shield_db_sqlite.cpp
)

target_include_directories(shield_db_sqlite
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/sqlite
)

find_package(unofficial-sqlite3 CONFIG REQUIRED)
target_link_libraries(shield_db_sqlite PRIVATE unofficial::sqlite3::sqlite3)

# Plugin DLLs sit alongside the host executable so the runtime loader can
# find them via host_executable_directory().
set_target_properties(shield_db_sqlite PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
)

# Standalone smoke test: loads the DLL directly and exercises the ABI.
# Not part of ctest by default - run manually to validate the plugin.
add_executable(shield_db_sqlite_smoke
    smoke_test.cpp
)
target_link_libraries(shield_db_sqlite PRIVATE shield_data)
target_link_libraries(shield_db_sqlite_smoke PRIVATE shield_data)
target_include_directories(shield_db_sqlite_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/src/data
)
add_dependencies(shield_db_sqlite_smoke shield_db_sqlite)

message(STATUS "  + shield_db_sqlite: SQLite backend plugin (with smoke test)")
