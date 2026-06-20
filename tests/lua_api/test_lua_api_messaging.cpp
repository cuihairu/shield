#define BOOST_TEST_MODULE LuaApiMessagingTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <nlohmann/json.hpp>
#include <string>

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

SpawnResult spawn_messaging(LuaServiceManager& manager, const std::string& name) {
    return manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                         opts_for(name).dump());
}
}  // namespace

BOOST_AUTO_TEST_SUITE(MessagingTests)

BOOST_AUTO_TEST_CASE(LAPI_004_01_SendByName) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto receiver = spawn_messaging(manager, "receiver_service");
    auto sender = spawn_messaging(manager, "sender_service");
    BOOST_REQUIRE(receiver.success);
    BOOST_REQUIRE(sender.success);

    CallResult send_result = manager.call(
        sender.service_id, "send_to",
        nlohmann::json::array({"receiver_service", "record", "test_message"}));
    BOOST_REQUIRE(send_result.success);
    BOOST_CHECK_EQUAL(send_result.values[0].get<bool>(), true);

    BOOST_REQUIRE(manager.process_mailbox(receiver.service_id));

    CallResult last = manager.call(receiver.service_id, "get_last_args",
                                   nlohmann::json::array());
    BOOST_REQUIRE(last.success);
    BOOST_REQUIRE_EQUAL(last.values.size(), 1u);
    BOOST_CHECK_EQUAL(last.values[0][0].get<std::string>(), "test_message");
}

BOOST_AUTO_TEST_CASE(LAPI_004_02_SendByHandleFromQuery) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto receiver = spawn_messaging(manager, "handle_receiver");
    auto sender = spawn_messaging(manager, "handle_sender");
    BOOST_REQUIRE(receiver.success);
    BOOST_REQUIRE(sender.success);

    CallResult send_result = manager.call(
        sender.service_id, "send_to_query",
        nlohmann::json::array({"handle_receiver", "record", "handle_test"}));
    BOOST_REQUIRE(send_result.success);
    BOOST_CHECK_EQUAL(send_result.values[0].get<bool>(), true);

    BOOST_REQUIRE(manager.process_mailbox(receiver.service_id));

    CallResult last = manager.call(receiver.service_id, "get_last_args",
                                   nlohmann::json::array());
    BOOST_REQUIRE(last.success);
    BOOST_CHECK_EQUAL(last.values[0][0].get<std::string>(), "handle_test");
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
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto receiver = spawn_messaging(manager, "method_receiver");
    auto sender = spawn_messaging(manager, "method_sender");
    BOOST_REQUIRE(receiver.success);
    BOOST_REQUIRE(sender.success);

    CallResult send_result = manager.call(
        sender.service_id, "send_to",
        nlohmann::json::array({"method_receiver", "missing_method"}));
    BOOST_REQUIRE(send_result.success);
    BOOST_CHECK_EQUAL(send_result.values[0].get<bool>(), true);

    BOOST_REQUIRE(manager.process_mailbox(receiver.service_id));

    CallResult last = manager.call(receiver.service_id, "get_last_method",
                                   nlohmann::json::array());
    BOOST_REQUIRE(last.success);
    BOOST_REQUIRE_EQUAL(last.values.size(), 1u);
    BOOST_CHECK(last.values[0].is_null());
}

BOOST_AUTO_TEST_CASE(LAPI_004_05_MailboxFull) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto receiver = spawn_messaging(manager, "slow_receiver");
    BOOST_REQUIRE(receiver.success);

    std::string error;
    bool mailbox_full = false;
    for (int i = 0; i < 1001; ++i) {
        if (!manager.send(receiver.service_id, "record",
                          nlohmann::json::array({i}), &error)) {
            mailbox_full = true;
            break;
        }
    }

    BOOST_CHECK(mailbox_full);
    BOOST_CHECK(error.find("mailbox full") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(LAPI_004_06_SelfSendIsQueued) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto self_sender = spawn_messaging(manager, "self_sender");
    BOOST_REQUIRE(self_sender.success);

    CallResult send_result = manager.call(
        self_sender.service_id, "send_to",
        nlohmann::json::array({"self_sender", "record", "self_message"}));
    BOOST_REQUIRE(send_result.success);
    BOOST_CHECK_EQUAL(send_result.values[0].get<bool>(), true);

    CallResult before = manager.call(self_sender.service_id, "get_last_method",
                                     nlohmann::json::array());
    BOOST_REQUIRE(before.success);
    BOOST_CHECK(before.values[0].is_null());

    BOOST_REQUIRE(manager.process_mailbox(self_sender.service_id));

    CallResult after = manager.call(self_sender.service_id, "get_last_method",
                                    nlohmann::json::array());
    BOOST_REQUIRE(after.success);
    BOOST_CHECK_EQUAL(after.values[0].get<std::string>(), "record");
}

BOOST_AUTO_TEST_SUITE_END()
