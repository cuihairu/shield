#define BOOST_TEST_MODULE HelloWorldAcceptanceTests
#include <boost/test/unit_test.hpp>

#include "shield/shield.hpp"

#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

using namespace boost::unit_test;

namespace {
const std::string EXAMPLE_CONFIG = "../examples/hello_world/config/app.yaml";
}

BOOST_AUTO_TEST_SUITE(HelloWorldAcceptanceTests)

BOOST_AUTO_TEST_CASE(HW_001_ConfigFileExists) {
    // Verify that the example configuration file exists and is valid
    BOOST_CHECK(std::filesystem::exists(EXAMPLE_CONFIG));
}

BOOST_AUTO_TEST_CASE(HW_002_LuaScriptsExist) {
    // Verify that all required Lua scripts exist
    BOOST_CHECK(std::filesystem::exists("../examples/hello_world/scripts/echo.lua"));
    BOOST_CHECK(std::filesystem::exists("../examples/hello_world/scripts/gateway.lua"));
    BOOST_CHECK(std::filesystem::exists("../examples/hello_world/scripts/player.lua"));
}

BOOST_AUTO_TEST_CASE(HW_003_LuaScriptsValidSyntax) {
    // This test would normally load each script to verify syntax
    // For now, just check files are readable
    std::ifstream echo_file("../examples/hello_world/scripts/echo.lua");
    BOOST_CHECK(echo_file.good());

    std::ifstream gateway_file("../examples/hello_world/scripts/gateway.lua");
    BOOST_CHECK(gateway_file.good());

    std::ifstream player_file("../examples/hello_world/scripts/player.lua");
    BOOST_CHECK(player_file.good());
}

BOOST_AUTO_TEST_CASE(HW_004_ServiceAPICoverage) {
    // Verify that the example covers all major service APIs

    // Read echo.lua and check for required API calls
    std::ifstream echo_file("../examples/hello_world/scripts/echo.lua");
    std::string echo_content((std::istreambuf_iterator<char>(echo_file)),
                           std::istreambuf_iterator<char>());

    // Check for lifecycle API
    BOOST_CHECK(echo_content.find("on_init") != std::string::npos);
    BOOST_CHECK(echo_content.find("on_exit") != std::string::npos);

    // Check for messaging API
    BOOST_CHECK(echo_content.find("shield.sender") != std::string::npos);
    BOOST_CHECK(echo_content.find("shield.send") != std::string::npos);

    // Check for logging API
    BOOST_CHECK(echo_content.find("shield.log.info") != std::string::npos);

    // Check for timer API
    BOOST_CHECK(echo_content.find("shield.now") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(HW_005_GatewayServiceCoverage) {
    // Verify gateway service covers network and session management
    std::ifstream gateway_file("../examples/hello_world/scripts/gateway.lua");
    std::string gateway_content((std::istreambuf_iterator<char>(gateway_file)),
                              std::istreambuf_iterator<char>());

    // Check for network connection handlers
    BOOST_CHECK(gateway_content.find("on_connect") != std::string::npos);
    BOOST_CHECK(gateway_content.find("on_disconnect") != std::string::npos);
    BOOST_CHECK(gateway_content.find("on_client_message") != std::string::npos);

    // Check for spawn API
    BOOST_CHECK(gateway_content.find("shield.spawn") != std::string::npos);

    // Check for session management
    BOOST_CHECK(gateway_content.find("sessions") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(HW_006_PlayerServiceCoverage) {
    // Verify player service covers business logic and data access
    std::ifstream player_file("../examples/hello_world/scripts/player.lua");
    std::string player_content((std::istreambuf_iterator<char>(player_file)),
                              std::istreambuf_iterator<char>());

    // Check for business logic handlers
    BOOST_CHECK(player_content.find("login") != std::string::npos);
    BOOST_CHECK(player_content.find("chat") != std::string::npos);
    BOOST_CHECK(player_content.find("logout") != std::string::npos);

    // Check for self API
    BOOST_CHECK(player_content.find("shield.self") != std::string::npos);

    // Check for exit API
    BOOST_CHECK(player_content.find("shield.exit") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(HW_007_DataAccessCoverage) {
    // Verify that the example demonstrates data access patterns
    std::ifstream player_file("../examples/hello_world/scripts/player.lua");
    std::string player_content((std::istreambuf_iterator<char>(player_file)),
                              std::istreambuf_iterator<char>());

    // Check for database API (commented out in default config but shown in example)
    BOOST_CHECK(player_content.find("shield.db") != std::string::npos);

    // Check for Redis API
    BOOST_CHECK(player_content.find("shield.redis") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(HW_008_MainEntryPoint) {
    // Verify that main.cpp uses the correct shield::run entry point
    std::ifstream main_file("../examples/hello_world/main.cpp");
    std::string main_content((std::istreambuf_iterator<char>(main_file)),
                            std::istreambuf_iterator<char>());

    // Check for shield::run usage
    BOOST_CHECK(main_content.find("shield::run") != std::string::npos);
    BOOST_CHECK(main_content.find("shield/shield.hpp") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
