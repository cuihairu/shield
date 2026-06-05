#define BOOST_TEST_MODULE DatabaseActorServiceTest
#include <boost/test/unit_test.hpp>

#include "shield/database/database_actor_service.hpp"

using namespace shield::database;

BOOST_AUTO_TEST_SUITE(DatabaseConfigTests)

BOOST_AUTO_TEST_CASE(TestDefaults) {
    DatabaseConfig config;
    BOOST_CHECK_EQUAL(config.driver, "mysql");
    BOOST_CHECK_EQUAL(config.host, "localhost");
    BOOST_CHECK_EQUAL(config.port, 3306);
    BOOST_CHECK(config.database.empty());
    BOOST_CHECK(config.username.empty());
    BOOST_CHECK(config.password.empty());
    BOOST_CHECK_EQUAL(config.max_connections, 10);
    BOOST_CHECK_EQUAL(config.connection_timeout, 30);
    BOOST_CHECK(config.auto_reconnect);
    BOOST_CHECK_EQUAL(config.charset, "utf8mb4");
}

BOOST_AUTO_TEST_CASE(TestCustomConfig) {
    DatabaseConfig config;
    config.driver = "postgresql";
    config.host = "db.example.com";
    config.port = 5432;
    config.database = "mydb";
    config.username = "user";
    config.password = "pass";
    config.max_connections = 20;
    config.connection_timeout = 60;
    config.auto_reconnect = false;
    config.charset = "utf8";

    BOOST_CHECK_EQUAL(config.driver, "postgresql");
    BOOST_CHECK_EQUAL(config.host, "db.example.com");
    BOOST_CHECK_EQUAL(config.port, 5432);
    BOOST_CHECK_EQUAL(config.database, "mydb");
    BOOST_CHECK_EQUAL(config.username, "user");
    BOOST_CHECK_EQUAL(config.password, "pass");
    BOOST_CHECK_EQUAL(config.max_connections, 20);
    BOOST_CHECK_EQUAL(config.connection_timeout, 60);
    BOOST_CHECK(!config.auto_reconnect);
    BOOST_CHECK_EQUAL(config.charset, "utf8");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(QueryResultTests)

BOOST_AUTO_TEST_CASE(TestDefaults) {
    QueryResult result;
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error.empty());
    BOOST_CHECK(result.rows.empty());
    BOOST_CHECK_EQUAL(result.affected_rows, 0u);
    BOOST_CHECK_EQUAL(result.last_insert_id, 0u);
}

BOOST_AUTO_TEST_CASE(TestWithData) {
    QueryResult result;
    result.success = true;
    result.affected_rows = 5;
    result.last_insert_id = 42;

    std::unordered_map<std::string, std::string> row;
    row["id"] = "1";
    row["name"] = "test";
    result.rows.push_back(row);

    BOOST_CHECK(result.success);
    BOOST_CHECK_EQUAL(result.affected_rows, 5u);
    BOOST_CHECK_EQUAL(result.last_insert_id, 42u);
    BOOST_CHECK_EQUAL(result.rows.size(), 1u);
    BOOST_CHECK_EQUAL(result.rows[0]["id"], "1");
    BOOST_CHECK_EQUAL(result.rows[0]["name"], "test");
}

BOOST_AUTO_TEST_CASE(TestMultipleRows) {
    QueryResult result;
    result.success = true;

    for (int i = 0; i < 10; i++) {
        std::unordered_map<std::string, std::string> row;
        row["id"] = std::to_string(i);
        result.rows.push_back(row);
    }

    BOOST_CHECK_EQUAL(result.rows.size(), 10u);
    BOOST_CHECK_EQUAL(result.rows[9]["id"], "9");
}

BOOST_AUTO_TEST_CASE(TestErrorResult) {
    QueryResult result;
    result.success = false;
    result.error = "Connection refused";

    BOOST_CHECK(!result.success);
    BOOST_CHECK_EQUAL(result.error, "Connection refused");
}

BOOST_AUTO_TEST_SUITE_END()
