# tools/shield-console - Interactive diagnostics CLI
cmake_minimum_required(VERSION 3.20)

find_package(replxx CONFIG REQUIRED)

add_executable(shield-console main.cpp)

target_link_libraries(shield-console
    PRIVATE
        Boost::asio
        nlohmann_json::nlohmann_json
        replxx::replxx
)

target_include_directories(shield-console
    PRIVATE ${PROJECT_SOURCE_DIR}/include
)

install(TARGETS shield-console RUNTIME DESTINATION bin)
