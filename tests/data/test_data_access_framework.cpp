#define BOOST_TEST_MODULE DataAccessFrameworkTest
#include <boost/test/unit_test.hpp>

#include "shield/data/data_access_framework.hpp"

using namespace shield::data;

BOOST_AUTO_TEST_SUITE(DataValueTests)

BOOST_AUTO_TEST_CASE(TestDefaultDataValue) {
    DataValue val;
    BOOST_CHECK(val.is_null());
    BOOST_CHECK_EQUAL(val.get_type(), DataValue::NULL_TYPE);
    BOOST_CHECK_EQUAL(val.to_string(), "NULL");
    BOOST_CHECK_EQUAL(val.to_json(), "null");
}

BOOST_AUTO_TEST_CASE(TestStringDataValue) {
    DataValue val(std::string("hello"));
    BOOST_CHECK_EQUAL(val.get_type(), DataValue::STRING);
    BOOST_CHECK(!val.is_null());
    BOOST_CHECK_EQUAL(val.to_string(), "hello");
    BOOST_CHECK_EQUAL(val.to_json(), "\"hello\"");
}

BOOST_AUTO_TEST_CASE(TestIntDataValue) {
    DataValue val(static_cast<int64_t>(42));
    BOOST_CHECK_EQUAL(val.get_type(), DataValue::INTEGER);
    BOOST_CHECK_EQUAL(val.as<int64_t>(), 42);
    BOOST_CHECK_EQUAL(val.to_string(), "42");
    BOOST_CHECK_EQUAL(val.to_json(), "42");
}

BOOST_AUTO_TEST_CASE(TestDoubleDataValue) {
    DataValue val(3.14);
    BOOST_CHECK_EQUAL(val.get_type(), DataValue::DOUBLE);
    BOOST_CHECK_EQUAL(val.as<double>(), 3.14);
    std::string s = val.to_string();
    BOOST_CHECK(s.find("3.14") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(TestBoolDataValue) {
    DataValue true_val(true);
    BOOST_CHECK_EQUAL(true_val.get_type(), DataValue::BOOLEAN);
    BOOST_CHECK_EQUAL(true_val.to_string(), "true");
    BOOST_CHECK_EQUAL(true_val.to_json(), "true");

    DataValue false_val(false);
    BOOST_CHECK_EQUAL(false_val.to_string(), "false");
    BOOST_CHECK_EQUAL(false_val.to_json(), "false");
}

BOOST_AUTO_TEST_CASE(TestObjectDataValueJson) {
    DataValue val;  // NULL type
    BOOST_CHECK_EQUAL(val.to_json(), "null");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(CriteriaTests)

BOOST_AUTO_TEST_CASE(TestCriteriaWhereEquals) {
    auto c = Criteria::where("name")->equals(DataValue(std::string("test")));
    BOOST_CHECK_EQUAL(c->get_field(), "name");
    BOOST_CHECK_EQUAL(c->get_operator(), Criteria::EQ);
    BOOST_CHECK_EQUAL(c->get_values().size(), 1u);
}

BOOST_AUTO_TEST_CASE(TestCriteriaNotEquals) {
    auto c = Criteria::where("id")->not_equals(DataValue(static_cast<int64_t>(1)));
    BOOST_CHECK_EQUAL(c->get_operator(), Criteria::NE);
}

BOOST_AUTO_TEST_CASE(TestCriteriaGreaterThan) {
    auto c = Criteria::where("age")->greater_than(DataValue(static_cast<int64_t>(18)));
    BOOST_CHECK_EQUAL(c->get_operator(), Criteria::GT);
}

BOOST_AUTO_TEST_CASE(TestCriteriaLessThan) {
    auto c = Criteria::where("score")->less_than(DataValue(static_cast<int64_t>(100)));
    BOOST_CHECK_EQUAL(c->get_operator(), Criteria::LT);
}

BOOST_AUTO_TEST_CASE(TestCriteriaLike) {
    auto c = Criteria::where("name")->like("%test%");
    BOOST_CHECK_EQUAL(c->get_operator(), Criteria::LIKE);
    BOOST_CHECK_EQUAL(c->get_values()[0].to_string(), "%test%");
}

BOOST_AUTO_TEST_CASE(TestCriteriaIn) {
    std::vector<DataValue> vals;
    vals.push_back(DataValue(static_cast<int64_t>(1)));
    vals.push_back(DataValue(static_cast<int64_t>(2)));
    vals.push_back(DataValue(static_cast<int64_t>(3)));
    auto c = Criteria::where("id")->in(vals);
    BOOST_CHECK_EQUAL(c->get_operator(), Criteria::IN);
    BOOST_CHECK_EQUAL(c->get_values().size(), 3u);
}

BOOST_AUTO_TEST_CASE(TestCriteriaIsNull) {
    auto c = Criteria::where("deleted_at")->is_null();
    BOOST_CHECK_EQUAL(c->get_operator(), Criteria::IS_NULL);
    BOOST_CHECK(c->get_values().empty());
}

BOOST_AUTO_TEST_CASE(TestCriteriaBetween) {
    auto c = Criteria::where("age")->between(
        DataValue(static_cast<int64_t>(18)),
        DataValue(static_cast<int64_t>(65)));
    BOOST_CHECK_EQUAL(c->get_operator(), Criteria::BETWEEN);
    BOOST_CHECK_EQUAL(c->get_values().size(), 2u);
}

BOOST_AUTO_TEST_CASE(TestCriteriaAndAlso) {
    auto c1 = Criteria::where("age")->greater_than(DataValue(static_cast<int64_t>(18)));
    auto c2 = Criteria::where("active")->equals(DataValue(true));
    auto combined = c1->and_also(c2);
    BOOST_CHECK_EQUAL(combined->get_operator(), Criteria::AND);
    BOOST_CHECK_EQUAL(combined->get_sub_criteria().size(), 2u);
}

BOOST_AUTO_TEST_CASE(TestCriteriaOrElse) {
    auto c1 = Criteria::where("role")->equals(DataValue(std::string("admin")));
    auto c2 = Criteria::where("role")->equals(DataValue(std::string("mod")));
    auto combined = c1->or_also(c2);
    BOOST_CHECK_EQUAL(combined->get_operator(), Criteria::OR);
    BOOST_CHECK_EQUAL(combined->get_sub_criteria().size(), 2u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(SortTests)

BOOST_AUTO_TEST_CASE(TestSortAsc) {
    Sort s = Sort::asc("name");
    BOOST_CHECK_EQUAL(s.field, "name");
    BOOST_CHECK_EQUAL(s.direction, Sort::ASC);
}

BOOST_AUTO_TEST_CASE(TestSortDesc) {
    Sort s = Sort::desc("created_at");
    BOOST_CHECK_EQUAL(s.field, "created_at");
    BOOST_CHECK_EQUAL(s.direction, Sort::DESC);
}

BOOST_AUTO_TEST_CASE(TestSortDefaultDirection) {
    Sort s("id");
    BOOST_CHECK_EQUAL(s.direction, Sort::ASC);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(PageableTests)

BOOST_AUTO_TEST_CASE(TestPageableDefaults) {
    Pageable p;
    BOOST_CHECK_EQUAL(p.page, 0u);
    BOOST_CHECK_EQUAL(p.size, 20u);
    BOOST_CHECK_EQUAL(p.get_offset(), 0u);
    BOOST_CHECK_EQUAL(p.get_limit(), 20u);
}

BOOST_AUTO_TEST_CASE(TestPageableCustom) {
    Pageable p(2, 50);
    BOOST_CHECK_EQUAL(p.page, 2u);
    BOOST_CHECK_EQUAL(p.size, 50u);
    BOOST_CHECK_EQUAL(p.get_offset(), 100u);
    BOOST_CHECK_EQUAL(p.get_limit(), 50u);
}

BOOST_AUTO_TEST_CASE(TestPageableWithSorts) {
    Pageable p(0, 10);
    p.sorts.push_back(Sort::desc("created_at"));
    BOOST_CHECK_EQUAL(p.sorts.size(), 1u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(QueryBuilderTests)

BOOST_AUTO_TEST_CASE(TestQueryBuilderBasic) {
    QueryBuilder qb("users");
    BOOST_CHECK_EQUAL(qb.get_collection(), "users");
    BOOST_CHECK(qb.get_select_fields().empty());
    BOOST_CHECK(!qb.get_criteria());
    BOOST_CHECK(qb.get_sorts().empty());
}

BOOST_AUTO_TEST_CASE(TestQueryBuilderSelect) {
    QueryBuilder qb("users");
    qb.select({"id", "name", "email"});
    BOOST_CHECK_EQUAL(qb.get_select_fields().size(), 3u);
    BOOST_CHECK_EQUAL(qb.get_select_fields()[0], "id");
}

BOOST_AUTO_TEST_CASE(TestQueryBuilderWhere) {
    QueryBuilder qb("users");
    auto criteria = Criteria::where("active")->equals(DataValue(true));
    qb.where(criteria);
    BOOST_CHECK(qb.get_criteria() != nullptr);
}

BOOST_AUTO_TEST_CASE(TestQueryBuilderOrderBy) {
    QueryBuilder qb("users");
    qb.order_by({Sort::asc("name"), Sort::desc("created_at")});
    BOOST_CHECK_EQUAL(qb.get_sorts().size(), 2u);
}

BOOST_AUTO_TEST_CASE(TestQueryBuilderLimit) {
    QueryBuilder qb("users");
    qb.limit(10);
    // limit is set, but we can't directly access it through the public API
    // We verify it doesn't crash
}

BOOST_AUTO_TEST_CASE(TestQueryBuilderOffset) {
    QueryBuilder qb("users");
    qb.offset(20);
}

BOOST_AUTO_TEST_CASE(TestQueryBuilderPage) {
    QueryBuilder qb("users");
    Pageable p(1, 25);
    qb.page(p);
    BOOST_CHECK_EQUAL(qb.get_sorts().size(), 0u);
}

BOOST_AUTO_TEST_CASE(TestQueryBuilderSetSingle) {
    QueryBuilder qb("users");
    qb.set("name", DataValue(std::string("test")));
    BOOST_CHECK_EQUAL(qb.get_updates().size(), 1u);
    BOOST_CHECK_EQUAL(qb.get_updates().at("name").to_string(), "test");
}

BOOST_AUTO_TEST_CASE(TestQueryBuilderSetMultiple) {
    QueryBuilder qb("users");
    std::unordered_map<std::string, DataValue> updates;
    updates["name"] = DataValue(std::string("test"));
    updates["age"] = DataValue(static_cast<int64_t>(25));
    qb.set(updates);
    BOOST_CHECK_EQUAL(qb.get_updates().size(), 2u);
}

BOOST_AUTO_TEST_CASE(TestQueryBuilderChain) {
    QueryBuilder qb("users");
    auto result = qb.select({"id", "name"})
                      .where(Criteria::where("active")->equals(DataValue(true)))
                      .order_by({Sort::asc("name")})
                      .limit(10)
                      .offset(0);
    // Chain returns reference, verify it compiles and works
    BOOST_CHECK_EQUAL(result.get_collection(), "users");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(QueryResultTests)

BOOST_AUTO_TEST_CASE(TestQueryResultDefaults) {
    QueryResult result;
    BOOST_CHECK(!result.success);
    BOOST_CHECK(result.error.empty());
    BOOST_CHECK(result.rows.empty());
    BOOST_CHECK_EQUAL(result.affected_rows, 0u);
    BOOST_CHECK(!result.last_insert_id.has_value());
}

BOOST_AUTO_TEST_CASE(TestQueryResultWithData) {
    QueryResult result;
    result.success = true;
    DataRow row;
    row["id"] = DataValue(static_cast<int64_t>(1));
    row["name"] = DataValue(std::string("test"));
    result.rows.push_back(row);
    result.affected_rows = 1;

    BOOST_CHECK(result.success);
    BOOST_CHECK_EQUAL(result.rows.size(), 1u);
    BOOST_CHECK_EQUAL(result.rows[0]["name"].to_string(), "test");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(DataSourceConfigTests)

BOOST_AUTO_TEST_CASE(TestDataSourceConfigDefaults) {
    DataSourceConfig config;
    BOOST_CHECK(config.host.empty() || config.host == "localhost");
    BOOST_CHECK_EQUAL(config.port, 0);
    BOOST_CHECK(config.database.empty());
    BOOST_CHECK_EQUAL(config.max_connections, 10);
    BOOST_CHECK_EQUAL(config.min_connections, 1);
    BOOST_CHECK_EQUAL(config.connection_timeout, 30);
    BOOST_CHECK(config.auto_reconnect);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(DataSourceFactoryTests)

BOOST_AUTO_TEST_CASE(TestRegisterAndCreate) {
    DataSourceFactory::register_creator(
        "test_mock", [](const DataSourceConfig& config) -> std::unique_ptr<IDataSource> {
            // Return nullptr for testing - we just test registration
            return nullptr;
        });

    auto types = DataSourceFactory::get_supported_types();
    bool found = false;
    for (const auto& t : types) {
        if (t == "test_mock") {
            found = true;
            break;
        }
    }
    BOOST_CHECK(found);
}

BOOST_AUTO_TEST_CASE(TestCreateUnsupportedType) {
    DataSourceConfig config;
    config.type = "nonexistent_type_xyz";
    BOOST_CHECK_THROW(DataSourceFactory::create(config), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
