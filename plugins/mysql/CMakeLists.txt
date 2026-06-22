# database.mysql — MySQL X DevAPI provider for shield.database.v1 (plugin system v1).
#
# Builds a runtime-loadable MODULE library implementing shield_plugin_get_v1.
add_library(shield_db_mysql MODULE shield_db_mysql.cpp)

target_include_directories(shield_db_mysql
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/mysql
)

find_package(unofficial-mysql-connector-cpp CONFIG REQUIRED)
target_link_libraries(shield_db_mysql PRIVATE unofficial::mysql-connector-cpp::connector)

set_target_properties(shield_db_mysql PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_db_mysql"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/database.mysql/bin"
)

file(COPY plugin.json
     DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/database.mysql")

message(STATUS "  + database.mysql: MySQL X DevAPI provider (v1 ABI)")
