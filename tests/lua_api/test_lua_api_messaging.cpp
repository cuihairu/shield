#define BOOST_TEST_MODULE LuaApiMessagingTests
#include <boost/test/unit_test.hpp>
#include <caf/actor_system.hpp>
#include <caf/actor_system_config.hpp>
#include <chrono>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "shield/caf_initializer.hpp"
#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";

nlohmann::json opts_for(const std::string& name) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", nlohmann::json::object()},
    };
}

SpawnResult spawn_messaging(LuaServiceManager& manager,
                            const std::string& name) {
    return manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                         opts_for(name).dump());
}

bool wait_until(std::function<bool()> predicate,
                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}
}  // namespace

struct CafInitFixture {
    CafInitFixture() { initialize_caf_types(); }
};
BOOST_GLOBAL_FIXTURE(CafInitFixture);

BOOST_AUTO_TEST_SUITE(MessagingTests)

BOOST_AUTO_TEST_CASE(LAPI_004_01_SendByName) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto receiver = spawn_messaging(manager, "receiver_service");
    auto sender = spawn_messaging(manager, "sender_service");
    BOOST_REQUIRE(receiver.success);
    BOOST_REQUIRE(sender.success);

    CallResult send_result = manager.call(
        sender.service_id, "send_to",
        nlohmann::json::array({"receiver_service", "record", "test_message"}));
    BOOST_REQUIRE(send_result.success);
    BOOST_CHECK_EQUAL(send_result.values[0].get<bool>(), true);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult last = manager.call(receiver.service_id, "get_last_args",
                                           nlohmann::json::array());
            return last.success && last.values.size() == 1u &&
                   last.values[0][0].get<std::string>() == "test_message";
        },
        std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(LAPI_004_02_SendByHandleFromQuery) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto receiver = spawn_messaging(manager, "handle_receiver");
    auto sender = spawn_messaging(manager, "handle_sender");
    BOOST_REQUIRE(receiver.success);
    BOOST_REQUIRE(sender.success);

    CallResult send_result = manager.call(
        sender.service_id, "send_to_query",
        nlohmann::json::array({"handle_receiver", "record", "handle_test"}));
    BOOST_REQUIRE(send_result.success);
    BOOST_CHECK_EQUAL(send_result.values[0].get<bool>(), true);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult last = manager.call(receiver.service_id, "get_last_args",
                                           nlohmann::json::array());
            return last.success && last.values.size() == 1u &&
                   last.values[0][0].get<std::string>() == "handle_test";
        },
        std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(LAPI_004_03_SendToMissingService) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto sender = spawn_messaging(manager, "sender_test");
    BOOST_REQUIRE(sender.success);

    CallResult send_result = manager.call(
        sender.service_id, "send_to",
        nlohmann::json::array({"missing_service", "record", "test"}));
    BOOST_REQUIRE(send_result.success);
    BOOST_REQUIRE_EQUAL(send_result.values.size(), 2u);
    BOOST_CHECK_EQUAL(send_result.values[0].get<bool>(), false);
    BOOST_CHECK_EQUAL(send_result.values[1]["code"].get<std::string>(),
                      "service_not_found");
}

BOOST_AUTO_TEST_CASE(LAPI_004_04_SendMissingMethodIsAcceptedButNotDispatched) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto receiver = spawn_messaging(manager, "method_receiver");
    auto sender = spawn_messaging(manager, "method_sender");
    BOOST_REQUIRE(receiver.success);
    BOOST_REQUIRE(sender.success);

    CallResult send_result = manager.call(
        sender.service_id, "send_to",
        nlohmann::json::array({"method_receiver", "missing_method"}));
    BOOST_REQUIRE(send_result.success);
    BOOST_CHECK_EQUAL(send_result.values[0].get<bool>(), true);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult last =
                manager.call(receiver.service_id, "get_last_method",
                             nlohmann::json::array());
            return last.success && last.values.size() == 1u &&
                   last.values[0].is_null();
        },
        std::chrono::seconds(1)));
}

// LAPI-004-05: MailboxFull test removed — CAF actor mailbox is unbounded.
// Backpressure is deferred per docs/runtime-messaging.md.

BOOST_AUTO_TEST_CASE(LAPI_004_06_SelfSendIsQueued) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto self_sender = spawn_messaging(manager, "self_sender");
    BOOST_REQUIRE(self_sender.success);

    CallResult send_result = manager.call(
        self_sender.service_id, "send_to",
        nlohmann::json::array({"self_sender", "record", "self_message"}));
    BOOST_REQUIRE(send_result.success);
    BOOST_CHECK_EQUAL(send_result.values[0].get<bool>(), true);

    // With CAF, the self-send goes through the actor's mailbox and gets
    // processed automatically. Wait for the message to be dispatched.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult after =
                manager.call(self_sender.service_id, "get_last_method",
                             nlohmann::json::array());
            return after.success && after.values.size() == 1u &&
                   after.values[0].get<std::string>() == "record";
        },
        std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_SUITE_END()
