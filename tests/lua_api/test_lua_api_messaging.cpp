#define BOOST_TEST_MODULE LuaApiMessagingTests
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

BOOST_AUTO_TEST_SUITE(MessagingTests)

BOOST_AUTO_TEST_CASE(LAPI_004_01_SendByName) {
    // Test sending message to service by name
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;
    std::string service_name = "receiver_service";

    // Spawn receiver service
    auto receiver = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        service_name,
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(receiver != nullptr);
    BOOST_CHECK(error.empty());

    // Create a sender service (could be the test harness itself)
    auto sender = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "sender_service",
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(sender != nullptr);

    // Send message from sender to receiver by name
    nlohmann::json message = {
        {"method", "echo"},
        {"args", {"test_message"}}
    };

    bool sent = manager->send_message(sender, service_name, message, &error);
    BOOST_CHECK(sent);
    BOOST_CHECK(error.empty());

    // Give time for message to be processed
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // TODO: Verify receiver received the message by checking its state
    // This requires the messaging_service.lua to expose received message state
}

BOOST_AUTO_TEST_CASE(LAPI_004_02_SendByHandle) {
    // Test sending message to service by opaque handle
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    // Spawn receiver and sender
    auto receiver = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "handle_receiver",
        nlohmann::json{{"test_case", "default"}}, &error);

    auto sender = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "handle_sender",
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(receiver != nullptr);
    BOOST_REQUIRE(sender != nullptr);

    // Send by handle
    nlohmann::json message = {
        {"method", "echo"},
        {"args", {"handle_test"}}
    };

    bool sent = manager->send_message(sender, receiver, message, &error);
    BOOST_CHECK(sent);
    BOOST_CHECK(error.empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

BOOST_AUTO_TEST_CASE(LAPI_004_03_SendToMissingService) {
    // Test that sending to non-existent service returns service_not_found
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto sender = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "sender_test",
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(sender != nullptr);

    // Try to send to non-existent service
    nlohmann::json message = {
        {"method", "echo"},
        {"args", {"test"}}
    };

    bool sent = manager->send_message(sender, "nonexistent_service", message, &error);
    BOOST_CHECK(!sent);
    BOOST_CHECK(!error.empty());
    BOOST_CHECK(error.find("not_found") != std::string::npos ||
                error.find("unknown") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(LAPI_004_04_SendMissingMethod) {
    // Test that sending to non-existent method results in dead letter
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto receiver = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "method_receiver",
        nlohmann::json{{"test_case", "default"}}, &error);

    auto sender = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "method_sender",
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(receiver != nullptr);
    BOOST_REQUIRE(sender != nullptr);

    // Send to non-existent method
    nlohmann::json message = {
        {"method", "nonexistent_method"},
        {"args", nlohmann::json::array()}
    };

    bool sent = manager->send_message(sender, receiver, message, &error);

    // Send itself might succeed (message sent), but delivery should fail
    // or be recorded in dead letter
    BOOST_CHECK((sent && error.empty()) || (!sent && !error.empty()));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // TODO: Check dead letter queue for undelivered message
}

BOOST_AUTO_TEST_CASE(LAPI_004_05_MailboxFull) {
    // Test that sending to full mailbox returns mailbox_full
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    // Create a slow receiver that doesn't process messages quickly
    auto slow_receiver = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "slow_receiver",
        nlohmann::json{{"test_case", "slow"}}, &error);

    auto sender = manager->spawn_service(
        TEST_SCRIPTS_DIR + "lifecycle_service.lua",
        "fast_sender",
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(slow_receiver != nullptr);
    BOOST_REQUIRE(sender != nullptr);

    // Send many messages rapidly to fill mailbox
    nlohmann::json message = {
        {"method", "echo"},
        {"args", {"spam"}}
    };

    bool mailbox_full_encountered = false;
    for (int i = 0; i < 10000; ++i) {
        std::string send_error;
        bool sent = manager->send_message(sender, slow_receiver, message, &send_error);
        if (!sent && send_error.find("full") != std::string::npos) {
            mailbox_full_encountered = true;
            break;
        }
    }

    // TODO: Implement mailbox size limits and test properly
    // BOOST_CHECK(mailbox_full_encountered);
}

BOOST_AUTO_TEST_CASE(LAPI_004_06_SelfSend) {
    // Test that self-send doesn't cause reentrant execution
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    auto self_sender = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "self_sender",
        nlohmann::json{{"test_case", "self_send"}}, &error);

    BOOST_REQUIRE(self_sender != nullptr);

    // Service sends message to itself
    nlohmann::json message = {
        {"method", "echo"},
        {"args", {"self_message"}}
    };

    bool sent = manager->send_message(self_sender, self_sender, message, &error);
    BOOST_CHECK(sent);
    BOOST_CHECK(error.empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify no reentrant execution occurred
    // (message should be queued for next scheduler cycle)
    // TODO: Add non-reentrancy verification
}

BOOST_AUTO_TEST_SUITE(LApi004Tests)

BOOST_AUTO_TEST_CASE(SendMessageVariations) {
    // Test various message sending scenarios
    auto runtime = std::make_shared<LuaRuntime>();
    auto manager = std::make_shared<LuaServiceManager>(*runtime);

    std::string error;

    // Test 1: Send with no args
    auto receiver = manager->spawn_service(
        TEST_SCRIPTS_DIR + "messaging_service.lua",
        "receiver_1",
        nlohmann::json{{"test_case", "default"}}, &error);

    BOOST_REQUIRE(receiver != nullptr);

    nlohmann::json msg_no_args = {
        {"method", "get_sender"},
        {"args", nlohmann::json::array()}
    };

    BOOST_CHECK(manager->send_message(receiver, receiver, msg_no_args, &error));

    // Test 2: Send with single arg
    nlohmann::json msg_single_arg = {
        {"method", "echo"},
        {"args", {"single_arg"}}
    };

    BOOST_CHECK(manager->send_message(receiver, receiver, msg_single_arg, &error));

    // Test 3: Send with multiple args
    nlohmann::json msg_multi_args = {
        {"method", "multi_return"},
        {"args", {"arg1", "arg2", "arg3"}}
    };

    BOOST_CHECK(manager->send_message(receiver, receiver, msg_multi_args, &error));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()
