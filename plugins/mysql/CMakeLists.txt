# MySQL plugin for shield_data.
#
# Wraps MySQL Connector/C++ (X DevAPI) behind the shield_db_plugin C ABI.
# This is the heavy backend: ships mysqlcppconnx + protobuf + abseil +
# friends inside the DLL. The host shield_data library never sees them.
add_library(shield_db_mysql SHARED
    shield_db_mysql.cpp
)

target_include_directories(shield_db_mysql
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/plugins/mysql
)

find_package(unofficial-mysql-connector-cpp CONFIG REQUIRED)
target_link_libraries(shield_db_mysql PRIVATE unofficial::mysql-connector-cpp::connector)

set_target_properties(shield_db_mysql PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
)

# vcpkg's applocal.ps1 will copy the connector's DLLs (mysqlcppconnx,
# protobuf, abseil, etc.) next to our plugin DLL on Windows. We also
# need them on PATH at runtime - the simplest fix is to keep the plugin
# DLL in the same directory as the host executable, which the runtime
# loader searches first.

message(STATUS "  + shield_db_mysql: MySQL X DevAPI backend plugin")
