#define BOOST_TEST_MODULE LuaApiGatewayTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <thread>
#include <chrono>

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";
}

BOOST_AUTO_TEST_SUITE(GatewayApiTests)

BOOST_AUTO_TEST_CASE(LAPI_009_01_SimulatedConnect) {
    // Test that gateway service handles simulated connect
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto gateway = manager->spawn_service(
        TEST_SCRIPTS_DIR + "gateway_service.lua",
        "gateway_connect_test",
        nlohmann::json{{"test_case", "connect"}}, &error);

    BOOST_REQUIRE(gateway != nullptr);

    // TODO: When session management is implemented:
    // 1. Create a mock session object
    // 2. Call gateway's on_connect handler
    // 3. Verify session is tracked
    // 4. Verify session.id is set
}

BOOST_AUTO_TEST_CASE(LAPI_009_02_ClientFrameDecoded) {
    // Test that decoded client frame delivers to on_client_message
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto gateway = manager->spawn_service(
        TEST_SCRIPTS_DIR + "gateway_service.lua",
        "gateway_message_test",
        nlohmann::json{{"test_case", "message"}}, &error);

    BOOST_REQUIRE(gateway != nullptr);

    // TODO: When message delivery is implemented:
    // 1. Create mock session with payload
    // 2. Deliver payload to gateway's on_client_message
    // 3. Verify handler received the payload
    // 4. Verify response was sent back
}

BOOST_AUTO_TEST_CASE(LAPI_009_03_DisconnectHandler) {
    // Test that close session calls on_disconnect with reason
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto gateway = manager->spawn_service(
        TEST_SCRIPTS_DIR + "gateway_service.lua",
        "gateway_disconnect_test",
        nlohmann::json{{"test_case", "disconnect"}}, &error);

    BOOST_REQUIRE(gateway != nullptr);

    // TODO: When session lifecycle is implemented:
    // 1. Create mock session
    // 2. Call on_connect
    // 3. Close session with reason
    // 4. Verify on_disconnect was called with correct reason
}

BOOST_AUTO_TEST_CASE(LAPI_009_04_SendQueueFull) {
    // Test that session:send returns error when queue full
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto gateway = manager->spawn_service(
        TEST_SCRIPTS_DIR + "gateway_service.lua",
        "gateway_queue_test",
        nlohmann::json{{"test_case", "queue_full"}}, &error);

    BOOST_REQUIRE(gateway != nullptr);

    // TODO: When send queue limiting is implemented:
    // 1. Create session with limited queue size
    // 2. Send messages until queue is full
    // 3. Verify next send returns false, session_send_queue_full
}

BOOST_AUTO_TEST_CASE(LAPI_009_05_StaleSessionSend) {
    // Test that send after session close returns session_closed
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto gateway = manager->spawn_service(
        TEST_SCRIPTS_DIR + "gateway_service.lua",
        "gateway_stale_test",
        nlohmann::json{{"test_case", "stale"}}, &error);

    BOOST_REQUIRE(gateway != nullptr);

    // TODO: When session state tracking is implemented:
    // 1. Create session
    // 2. Close session
    // 3. Try to send on closed session
    // 4. Verify result: false, session_closed
}

BOOST_AUTO_TEST_CASE(SessionAPIBasics) {
    // Test that session API has correct structure
    auto runtime = std::make_shared<LuaRuntime>();

    auto vm = runtime->create_vm();
    runtime->register_api(vm);

    // TODO: When session API is implemented, verify:
    // - Session object has :send() method
    // - Session object has :close() method
    // - Session object has :remote_address() method
    // - Session object has :id property
}

BOOST_AUTO_TEST_CASE(GatewayServicePattern) {
    // Test that gateway can spawn services per connection
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto gateway = manager->spawn_service(
        TEST_SCRIPTS_DIR + "gateway_service.lua",
        "gateway_spawn_test",
        nlohmann::json{{"test_case", "spawn_per_connection"}}, &error);

    BOOST_REQUIRE(gateway != nullptr);

    // TODO: When gateway pattern is implemented:
    // 1. Simulate multiple connections
    // 2. Verify gateway spawns service for each connection
    // 3. Verify services are tracked per session
}

BOOST_AUTO_TEST_SUITE_END()
