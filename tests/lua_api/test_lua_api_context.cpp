#define BOOST_TEST_MODULE LuaApiContextTests
#include <boost/test/unit_test.hpp>

#include "shield/lua/lua_runtime.hpp"
#include "shield/lua/lua_service.hpp"

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

using namespace shield::lua;

namespace {
const std::string TEST_SCRIPTS_DIR = "../tests/lua_api/scripts/";

nlohmann::json opts_for(const std::string& name,
                        nlohmann::json config = nlohmann::json::object()) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", std::move(config)},
    };
}

SpawnResult spawn_messaging(LuaServiceManager& manager,
                            const std::string& name,
                            nlohmann::json config = nlohmann::json::object()) {
    return manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                         opts_for(name, std::move(config)).dump());
}
}  // namespace

BOOST_AUTO_TEST_SUITE(ContextTests)

BOOST_AUTO_TEST_CASE(LAPI_006_01_SenderInMessageHandler) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto receiver = spawn_messaging(manager, "sender_receiver");
    auto sender = spawn_messaging(manager, "sender_service");
    BOOST_REQUIRE(receiver.success);
    BOOST_REQUIRE(sender.success);

    BOOST_REQUIRE(manager.send(receiver.service_id, "record",
                               nlohmann::json::array({"payload"})));
    BOOST_REQUIRE(manager.process_mailbox(receiver.service_id));

    CallResult cr = manager.call(receiver.service_id, "get_last_sender",
                                 nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(cr.values.size(), 1u);
    BOOST_CHECK(cr.values[0].is_null());

    CallResult send_result = manager.call(
        sender.service_id, "send_to",
        nlohmann::json::array({"sender_receiver", "record", "from_sender"}));
    BOOST_REQUIRE(send_result.success);
    BOOST_REQUIRE(manager.process_mailbox(receiver.service_id));

    cr = manager.call(receiver.service_id, "get_last_sender",
                      nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    BOOST_CHECK_EQUAL(cr.values[0].get<std::string>(), sender.service_id);
}

BOOST_AUTO_TEST_CASE(LAPI_006_02_SenderAfterHandlerReturnsIsCleared) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto receiver = spawn_messaging(manager, "context_test");
    auto sender = spawn_messaging(manager, "temp_sender");
    BOOST_REQUIRE(receiver.success);
    BOOST_REQUIRE(sender.success);

    CallResult send_result = manager.call(
        sender.service_id, "send_to",
        nlohmann::json::array({"context_test", "save_sender_reader"}));
    BOOST_REQUIRE(send_result.success);
    BOOST_REQUIRE(manager.process_mailbox(receiver.service_id));

    CallResult saved = manager.call(receiver.service_id, "read_saved_sender",
                                    nlohmann::json::array());
    BOOST_REQUIRE(saved.success);
    BOOST_REQUIRE_EQUAL(saved.values.size(), 1u);
    BOOST_CHECK(saved.values[0].is_null());
}

BOOST_AUTO_TEST_CASE(LAPI_006_03_SenderInTimerCallbackIsNil) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto service = spawn_messaging(manager, "timer_sender_test",
                                   {{"test_case", "timer_sender"}});
    BOOST_REQUIRE(service.success);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    (void)manager.pump_once();

    CallResult cr = manager.call(service.service_id, "get_last_sender",
                                 nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(cr.values.size(), 1u);
    BOOST_CHECK_EQUAL(cr.values[0].get<std::string>(), "__nil__");
}

BOOST_AUTO_TEST_CASE(LAPI_006_04_SelfContext) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto service = spawn_messaging(manager, "self_test_service");
    BOOST_REQUIRE(service.success);

    CallResult cr = manager.call(service.service_id, "self_id",
                                 nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(cr.values.size(), 1u);
    BOOST_CHECK_EQUAL(cr.values[0].get<std::string>(), service.service_id);
}

BOOST_AUTO_TEST_CASE(LAPI_006_05_ContextIsolation) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto receiver = spawn_messaging(manager, "isolation_receiver");
    auto sender1 = spawn_messaging(manager, "sender1");
    auto sender2 = spawn_messaging(manager, "sender2");
    BOOST_REQUIRE(receiver.success);
    BOOST_REQUIRE(sender1.success);
    BOOST_REQUIRE(sender2.success);

    BOOST_REQUIRE(manager.call(sender1.service_id, "send_to",
                               nlohmann::json::array({"isolation_receiver",
                                                      "record", "one"}))
                      .success);
    BOOST_REQUIRE(manager.process_mailbox(receiver.service_id));
    CallResult first = manager.call(receiver.service_id, "get_last_sender",
                                    nlohmann::json::array());
    BOOST_REQUIRE(first.success);
    BOOST_CHECK_EQUAL(first.values[0].get<std::string>(), sender1.service_id);

    BOOST_REQUIRE(manager.call(sender2.service_id, "send_to",
                               nlohmann::json::array({"isolation_receiver",
                                                      "record", "two"}))
                      .success);
    BOOST_REQUIRE(manager.process_mailbox(receiver.service_id));
    CallResult second = manager.call(receiver.service_id, "get_last_sender",
                                     nlohmann::json::array());
    BOOST_REQUIRE(second.success);
    BOOST_CHECK_EQUAL(second.values[0].get<std::string>(), sender2.service_id);
}

BOOST_AUTO_TEST_SUITE_END()
