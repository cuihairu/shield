# database.mongodb — mongocxx provider for shield.document.v1 (plugin system v1).
#
# Builds a runtime-loadable MODULE library implementing shield_plugin_get_v1.
# Deployed to <runtime>/plugins/database.mongodb/{manifest.yaml,bin/...} so
# PluginHost's scan() finds the manifest and load() maps the library.
#
# This plugin is fully Lua-autonomous: register_lua() installs the callable
# shield.database.mongodb namespace from this DLL, not from the host. sol2
# and Lua are PRIVATE dependencies — their symbols must NOT leak into the
# host process (sol2 is header-only but needs Lua headers at compile time
# and lua52/53/luajit import library at link time).
#
# mongocxx (and its C dependency libmongoc) are PRIVATELY linked here as
# well: the host never links mongo-cxx-driver, so the BSON/Mongo types stay
# inside this plugin DLL.
add_library(shield_doc_mongodb MODULE
    shield_doc_mongodb.cpp
)

target_include_directories(shield_doc_mongodb
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/_shared
        ${PROJECT_SOURCE_DIR}/plugins/mongodb
        ${LUA_INCLUDE_DIR}
        ${SOL2_INCLUDE_DIRS}
)

find_package(mongocxx REQUIRED)
target_link_libraries(shield_doc_mongodb
    PRIVATE
        mongo::mongocxx_shared
        sol2::sol2
        ${LUA_LIBRARIES}
)

set_target_properties(shield_doc_mongodb PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_doc_mongodb"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/database.mongodb/bin"
)

file(COPY manifest.yaml
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/database.mongodb")
file(COPY lua/
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/database.mongodb/lua")

message(STATUS "  + database.mongodb: MongoDB (mongocxx) provider (v1 ABI, Lua-autonomous)")
