#define BOOST_TEST_MODULE LuaApiContextTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <filesystem>
#include <thread>
#include <chrono>

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";
}

BOOST_AUTO_TEST_SUITE(ContextTests)

BOOST_AUTO_TEST_CASE(LAPI_006_01_SenderInMessageHandler) {
    // Test that shield.sender() in message handler returns caller's handle
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto receiver = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "sender_receiver",
        nlohmann::json{{"test_case", "check_sender"}}, &error);

    auto sender = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "sender_service",
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(receiver != nullptr);
    BOOST_REQUIRE(sender != nullptr);

    // Send message from sender to receiver
    nlohmann::json message = {
        {"method", "check_sender"},
        {"args", nlohmann::json::array()}
    };

    bool sent = manager->send_message(sender, receiver, message, &error);
    BOOST_CHECK(sent);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // TODO: Verify that receiver's shield.sender() returned sender's handle
    // This requires the messaging_service.lua to report back the sender it received
}

BOOST_AUTO_TEST_CASE(LAPI_006_02_SenderAfterHandlerReturns) {
    // Test that saved callback reading sender returns nil or context_expired
    // This tests that sender context is not leaked beyond the handler scope
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "context_test",
        nlohmann::json{{"test_case", "save_sender"}}, &error);

    // Send message and let handler complete
    nlohmann::json message = {
        {"method", "save_and_check_sender"},
        {"args", nlohmann::json::array()}
    };

    auto sender = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "temp_sender",
        nlohmann::json{{"test_case", "default"}}, &error);

    manager->send_message(sender, service, message, &error);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // After handler returns, saved sender should be nil or expired
    // TODO: Implement verification that saved sender context is cleared
}

BOOST_AUTO_TEST_CASE(LAPI_006_03_SenderInTimerCallback) {
    // Test that shield.sender() in timer callback returns nil
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "timer_sender_test",
        nlohmann::json{{"test_case", "timer_sender"}}, &error);

    // TODO: Implement timer scheduling and test shield.sender() in timer callback
    // For now, just verify service spawned
    BOOST_CHECK(service != nullptr);
}

BOOST_AUTO_TEST_CASE(LAPI_006_04_SelfContext) {
    // Test that shield.self() returns the current service's handle
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto service = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "self_test_service",
        nlohmann::json{{"test_case", "check_self"}}, &error);

    BOOST_REQUIRE(service != nullptr);

    // Query the service
    auto queried = runtime->query_service("self_test_service");
    BOOST_CHECK(queried != nullptr);

    // TODO: Implement test that verifies shield.self() equals the queried handle
    // This requires Lua-side testing of shield.self()
}

BOOST_AUTO_TEST_CASE(LAPI_006_05_ContextIsolation) {
    // Test that sender context is isolated between different message handlers
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto receiver = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "isolation_receiver",
        nlohmann::json{{"test_case", "isolation"}}, &error);

    auto sender1 = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "sender1",
        nlohmann::json{{"test_case", "default"}}, &error);

    auto sender2 = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "sender2",
        nlohmann::json{{"test_case", "default"}}, &error);

    // Send messages from different senders
    nlohmann::json msg1 = {{"method", "check_sender"}, {"args", nlohmann::json::array()}};
    nlohmann::json msg2 = {{"method", "check_sender"}, {"args", nlohmann::json::array()}};

    manager->send_message(sender1, receiver, msg1, &error);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    manager->send_message(sender2, receiver, msg2, &error);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Each handler should see its own sender context, not the previous one
    // TODO: Implement context isolation verification
}

BOOST_AUTO_TEST_SUITE_END()
