# database.mysql — MySQL provider for shield.database.v1 (plugin system v1).
#
# Built on MariaDB Connector/C (libmariadb), which speaks the classic MySQL
# client/server protocol and connects to both MySQL and MariaDB servers.
#
# Builds a runtime-loadable MODULE library implementing shield_plugin_get_v1.
# Deployed to <runtime>/plugins/database.mysql/{manifest.yaml,bin/...} so
# PluginHost's scan() finds the manifest and load() maps the library.
#
# This plugin owns its Lua autonomy: register_lua() builds the callable
# shield.database.mysql namespace from this DLL, not from the host. sol2 and
# Lua are therefore PRIVATE dependencies — their symbols must NOT leak into
# the host process (sol2 is header-only but needs Lua's headers at compile
# time and lua52/53/luajit import library at link time).
add_library(shield_db_mysql MODULE shield_db_mysql.cpp)

target_include_directories(shield_db_mysql
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/_shared
        ${PROJECT_SOURCE_DIR}/plugins/mysql
        ${LUA_INCLUDE_DIR}
        ${SOL2_INCLUDE_DIRS}
)

find_package(unofficial-libmariadb CONFIG REQUIRED)
# The MariaDB Connector/C target declares its transitive deps (z/ssl/crypto)
# itself — unlike mysql-connector-cpp, no manual link patching is needed.
target_link_libraries(shield_db_mysql
    PRIVATE
        unofficial::libmariadb
        sol2::sol2
        ${LUA_LIBRARIES}
)

set_target_properties(shield_db_mysql PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_db_mysql"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/database.mysql/bin"
)

file(COPY manifest.yaml
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/database.mysql")

message(STATUS "  + database.mysql: MySQL X DevAPI provider (v1 ABI, Lua-autonomous, per-instance pool)")
