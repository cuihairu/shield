# database.sqlite — SQLite provider for shield.database.v1 (plugin system v1).
#
# Builds a runtime-loadable MODULE library implementing shield_plugin_get_v1.
# Deployed to <runtime>/plugins/database.sqlite/{plugin.json,bin/...} so
# PluginHost's scan() finds the manifest and load() maps the library.
#
# This plugin owns its Lua autonomy: register_lua() builds the callable
# shield.database.sqlite namespace from this DLL, not from the host. sol2 and
# Lua are therefore PRIVATE dependencies — their symbols must NOT leak into
# the host process (sol2 is header-only but needs Lua's headers at compile
# time and lua52/53/luajit import library at link time).
add_library(shield_database_sqlite MODULE shield_db_sqlite.cpp)

target_include_directories(shield_database_sqlite
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/_shared
        ${LUA_INCLUDE_DIR}
        ${SOL2_INCLUDE_DIRS}
)

find_package(unofficial-sqlite3 CONFIG REQUIRED)
target_link_libraries(shield_database_sqlite
    PRIVATE
        unofficial::sqlite3::sqlite3
        sol2::sol2
        ${LUA_LIBRARIES}
)

# Package layout: plugins/database.sqlite/{plugin.json, bin/lib...}
set_target_properties(shield_database_sqlite PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_database_sqlite"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/database.sqlite/bin"
)

# Copy the manifest next to the binary so scan() finds it.
file(COPY plugin.json
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/database.sqlite")

message(STATUS "  + database.sqlite: SQLite provider (v1 ABI, Lua-autonomous)")
