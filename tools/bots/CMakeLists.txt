# tools/bots - load testing and boundary-discovery bot clients
#
# The core is a static library so the acceptance smoke test can drive the
# same code path in-process; the `bots` executable is the operator CLI
# (run a load plan / inject boundary faults / single round-trip probe).
#
# Zero shield libraries: the bot speaks the wire protocol directly
# (idlen framing + JSON body), mirroring an external game client.
add_library(shield_bots_core STATIC
    bot_client.cpp
    plan.cpp
    load_runner.cpp
    chaos.cpp
)

target_link_libraries(shield_bots_core
    PUBLIC
        Boost::asio
        nlohmann_json::nlohmann_json
        yaml-cpp::yaml-cpp
)

target_include_directories(shield_bots_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(shield_bots_core
    PUBLIC SHIELD_BOTS_VERSION_STRING="${PROJECT_VERSION}"
)

add_executable(bots main.cpp)

target_link_libraries(bots
    PRIVATE
        shield_bots_core
        Boost::headers
)

install(TARGETS bots RUNTIME DESTINATION bin)
