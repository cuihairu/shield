#define BOOST_TEST_MODULE LuaApiConcurrencyTests
#include <atomic>
#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

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

SpawnResult spawn_service(LuaServiceManager& manager, const std::string& name) {
    return manager.spawn(TEST_SCRIPTS_DIR + "messaging_service.lua",
                         opts_for(name).dump());
}
}  // namespace

BOOST_AUTO_TEST_SUITE(ConcurrencyTests)

BOOST_AUTO_TEST_CASE(DispatchContextIsThreadLocal) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    auto result = spawn_service(manager, "thread_context_service");
    BOOST_REQUIRE(result.success);

    std::string worker_service_id;
    std::thread worker([&]() {
        manager.enqueue_forked_task(result.service_id, [&]() {
            worker_service_id = manager.current_service_id();
        });
        BOOST_CHECK_EQUAL(manager.pump_once(), 1);
    });

    BOOST_CHECK_EQUAL(manager.current_service_id(), "");
    worker.join();
    BOOST_CHECK_EQUAL(worker_service_id, result.service_id);
    BOOST_CHECK_EQUAL(manager.current_service_id(), "");
}

BOOST_AUTO_TEST_CASE(ConcurrentQueryAndListAreStable) {
    LuaRuntime runtime;
    LuaServiceManager manager(runtime);

    std::vector<std::string> names;
    for (int i = 0; i < 8; ++i) {
        const std::string name = "concurrent_query_" + std::to_string(i);
        auto result = spawn_service(manager, name);
        BOOST_REQUIRE(result.success);
        names.push_back(name);
    }

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&]() {
            while (!start.load()) {
            }
            for (int i = 0; i < 500; ++i) {
                for (const auto& name : names) {
                    if (manager.query_service(name) != name) {
                        failed.store(true);
                    }
                }
                auto listed = manager.list_services();
                if (listed.size() < names.size()) {
                    failed.store(true);
                }
            }
        });
    }

    start.store(true);
    for (auto& reader : readers) {
        reader.join();
    }

    BOOST_CHECK(!failed.load());
}

BOOST_AUTO_TEST_SUITE_END()
