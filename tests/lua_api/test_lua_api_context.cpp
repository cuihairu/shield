#define BOOST_TEST_MODULE LuaApiContextTests
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

nlohmann::json opts_for(const std::string& name,
                        nlohmann::json config = nlohmann::json::object()) {
    return {
        {"name", name},
        {"args", nlohmann::json::object()},
        {"config", std::move(config)},
    };
}

SpawnResult spawn_messaging(LuaServiceManager& manager, const std::string& name,
                            nlohmann::json config = nlohmann::json::object()) {
    return manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                         opts_for(name, std::move(config)).dump());
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

BOOST_AUTO_TEST_SUITE(ContextTests)

BOOST_AUTO_TEST_CASE(LAPI_006_01_SenderInMessageHandler) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto receiver = spawn_messaging(manager, "sender_receiver");
    auto sender = spawn_messaging(manager, "sender_service");
    BOOST_REQUIRE(receiver.success);
    BOOST_REQUIRE(sender.success);

    BOOST_REQUIRE(manager.send(receiver.service_id, "record",
                               nlohmann::json::array({"payload"})));

    // With CAF, the send goes through the actor automatically.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult cr = manager.call(receiver.service_id, "get_last_sender",
                                         nlohmann::json::array());
            return cr.success && cr.values.size() == 1u &&
                   cr.values[0].is_null();
        },
        std::chrono::seconds(1)));

    CallResult send_result = manager.call(
        sender.service_id, "send_to",
        nlohmann::json::array({"sender_receiver", "record", "from_sender"}));
    BOOST_REQUIRE(send_result.success);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult cr = manager.call(receiver.service_id, "get_last_sender",
                                         nlohmann::json::array());
            return cr.success && cr.values.size() == 1u &&
                   cr.values[0].get<std::string>() == sender.service_id;
        },
        std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(LAPI_006_02_SenderAfterHandlerReturnsIsCleared) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto receiver = spawn_messaging(manager, "context_test");
    auto sender = spawn_messaging(manager, "temp_sender");
    BOOST_REQUIRE(receiver.success);
    BOOST_REQUIRE(sender.success);

    CallResult send_result = manager.call(
        sender.service_id, "send_to",
        nlohmann::json::array({"context_test", "save_sender_reader"}));
    BOOST_REQUIRE(send_result.success);

    BOOST_CHECK(wait_until(
        [&]() {
            CallResult saved =
                manager.call(receiver.service_id, "read_saved_sender",
                             nlohmann::json::array());
            return saved.success && saved.values.size() == 1u &&
                   saved.values[0].is_null();
        },
        std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_CASE(LAPI_006_03_SenderInTimerCallbackIsNil) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto service = spawn_messaging(manager, "timer_sender_test",
                                   {{"test_case", "timer_sender"}});
    BOOST_REQUIRE(service.success);

    // With CAF, the timer fires automatically through the actor.
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult cr = manager.call(service.service_id, "get_last_sender",
                                         nlohmann::json::array());
            if (!cr.success || cr.values.size() < 1u) {
                return false;
            }
            // With CAF, shield.sender() in a timer callback returns null
            // (not Lua nil). The Lua code stores it as-is (null or "__nil__").
            // Accept either null or "__nil__".
            return cr.values[0].is_null() ||
                   (cr.values[0].is_string() &&
                    cr.values[0].get<std::string>() == "__nil__");
        },
        std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(LAPI_006_04_SelfContext) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto service = spawn_messaging(manager, "self_test_service");
    BOOST_REQUIRE(service.success);

    CallResult cr =
        manager.call(service.service_id, "self_id", nlohmann::json::array());
    BOOST_REQUIRE(cr.success);
    BOOST_REQUIRE_EQUAL(cr.values.size(), 1u);
    BOOST_CHECK_EQUAL(cr.values[0].get<std::string>(), service.service_id);
}

BOOST_AUTO_TEST_CASE(LAPI_006_05_ContextIsolation) {
    caf::actor_system_config cfg;
    caf::actor_system system(cfg);

    LuaRuntime runtime;
    LuaServiceManager manager(runtime);
    manager.attach_actor_system(system);

    auto receiver = spawn_messaging(manager, "isolation_receiver");
    auto sender1 = spawn_messaging(manager, "sender1");
    auto sender2 = spawn_messaging(manager, "sender2");
    BOOST_REQUIRE(receiver.success);
    BOOST_REQUIRE(sender1.success);
    BOOST_REQUIRE(sender2.success);

    BOOST_REQUIRE(manager
                      .call(sender1.service_id, "send_to",
                            nlohmann::json::array(
                                {"isolation_receiver", "record", "one"}))
                      .success);
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult first =
                manager.call(receiver.service_id, "get_last_sender",
                             nlohmann::json::array());
            return first.success && first.values.size() == 1u &&
                   first.values[0].get<std::string>() == sender1.service_id;
        },
        std::chrono::seconds(1)));

    BOOST_REQUIRE(manager
                      .call(sender2.service_id, "send_to",
                            nlohmann::json::array(
                                {"isolation_receiver", "record", "two"}))
                      .success);
    BOOST_CHECK(wait_until(
        [&]() {
            CallResult second =
                manager.call(receiver.service_id, "get_last_sender",
                             nlohmann::json::array());
            return second.success && second.values.size() == 1u &&
                   second.values[0].get<std::string>() == sender2.service_id;
        },
        std::chrono::seconds(1)));
}

BOOST_AUTO_TEST_SUITE_END()
