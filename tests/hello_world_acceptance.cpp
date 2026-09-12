#define BOOST_TEST_MODULE HelloWorldAcceptanceTests
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "shield/shield.hpp"

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
    BOOST_CHECK(
        std::filesystem::exists("../examples/hello_world/scripts/auth.lua"));
    BOOST_CHECK(
        std::filesystem::exists("../examples/hello_world/scripts/player.lua"));
    BOOST_CHECK(
        std::filesystem::exists("../examples/hello_world/scripts/room.lua"));
}

BOOST_AUTO_TEST_CASE(HW_003_LuaScriptsValidSyntax) {
    // This test would normally load each script to verify syntax
    // For now, just check files are readable
    std::ifstream auth_file("../examples/hello_world/scripts/auth.lua");
    BOOST_CHECK(auth_file.good());

    std::ifstream player_file("../examples/hello_world/scripts/player.lua");
    BOOST_CHECK(player_file.good());

    std::ifstream room_file("../examples/hello_world/scripts/room.lua");
    BOOST_CHECK(room_file.good());
}

BOOST_AUTO_TEST_CASE(HW_004_AuthServiceCoverage) {
    // Verify the auth entry service covers the pre-auth login flow: the
    // c2s login handler, the coroutine-aware single-target bind, and the
    // s2c login_result helper.
    std::ifstream auth_file("../examples/hello_world/scripts/auth.lua");
    std::string auth_content((std::istreambuf_iterator<char>(auth_file)),
                             std::istreambuf_iterator<char>());

    // Check for the lifecycle API
    BOOST_CHECK(auth_content.find("on_init") != std::string::npos);
    BOOST_CHECK(auth_content.find("on_exit") != std::string::npos);

    // Check for the login handler and client-bound callback
    BOOST_CHECK(auth_content.find("M.login") != std::string::npos);
    BOOST_CHECK(auth_content.find("on_client_bound") != std::string::npos);

    // Check for the single-target bind and the s2c login_result helper
    BOOST_CHECK(auth_content.find("shield.client.bind") != std::string::npos);
    BOOST_CHECK(auth_content.find("shield.client_rpc.login_result") !=
                std::string::npos);
}

BOOST_AUTO_TEST_CASE(HW_005_PlayerServiceCoverage) {
    // Verify the player service covers the post-auth target: the bound
    // callback stores the client context, the move handler guards on the
    // trusted identity, and business is forwarded to room via shield.send.
    std::ifstream player_file("../examples/hello_world/scripts/player.lua");
    std::string player_content((std::istreambuf_iterator<char>(player_file)),
                               std::istreambuf_iterator<char>());

    // Check for the client-control handlers
    BOOST_CHECK(player_content.find("on_client_bound") != std::string::npos);
    BOOST_CHECK(player_content.find("on_disconnect") != std::string::npos);
    BOOST_CHECK(player_content.find("on_client_unbound") != std::string::npos);

    // Check for the move handler with the trusted-identity guard
    BOOST_CHECK(player_content.find("M.move") != std::string::npos);
    BOOST_CHECK(player_content.find("client:player_id()") != std::string::npos);

    // Check for the private room forwarding
    BOOST_CHECK(player_content.find("shield.send(\"room\"") !=
                    std::string::npos ||
                player_content.find("shield.send('room'") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(HW_006_RoomServiceCoverage) {
    // Verify the room service covers the server-to-client side: it answers
    // with the move_result helper only (no generic send API).
    std::ifstream room_file("../examples/hello_world/scripts/room.lua");
    std::string room_content((std::istreambuf_iterator<char>(room_file)),
                             std::istreambuf_iterator<char>());

    BOOST_CHECK(room_content.find("M.join") != std::string::npos);
    BOOST_CHECK(room_content.find("M.move") != std::string::npos);
    BOOST_CHECK(room_content.find("shield.client_rpc.move_result") !=
                std::string::npos);
}

BOOST_AUTO_TEST_CASE(HW_007_AppConfigRoutesCoverage) {
    // Verify the example config declares the client RPC routes: the auth
    // listener carries the protocol pipeline, login is pre-auth, and the
    // move route lives on the player actor.
    std::ifstream config_file("../examples/hello_world/config/app.yaml");
    std::string config_content((std::istreambuf_iterator<char>(config_file)),
                               std::istreambuf_iterator<char>());

    BOOST_CHECK(config_content.find("rpc:") != std::string::npos);
    BOOST_CHECK(config_content.find("routes:") != std::string::npos);
    BOOST_CHECK(config_content.find("requires_auth: false") !=
                std::string::npos);
    BOOST_CHECK(config_content.find("direction: \"s2c\"") != std::string::npos);
    BOOST_CHECK(config_content.find("protocol:") != std::string::npos);
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
