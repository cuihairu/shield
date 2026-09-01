#define BOOST_TEST_MODULE CovSchemaValidator
#include <algorithm>
#include <boost/test/unit_test.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "plugin/schema_validator.hpp"

using namespace shield::plugin;
using json = nlohmann::json;

// Unknown "type" keywords are accepted leniently (forward compatibility).
BOOST_AUTO_TEST_CASE(unknown_type_keyword_is_lenient) {
    json schema = {{"type", "whatever"}};
    json value = "anything";
    BOOST_CHECK(validate_config(schema, value).empty());
}

// Numeric bounds: below minimum fails, in-range passes.
BOOST_AUTO_TEST_CASE(below_minimum_fails) {
    json schema = {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}};
    BOOST_CHECK(!validate_config(schema, json(0)).empty());
    BOOST_CHECK(validate_config(schema, json(50)).empty());
}

// Enum membership: a matching value passes, others fail.
BOOST_AUTO_TEST_CASE(enum_match_passes) {
    json schema = {{"enum", json::array({"a", "b"})}};
    BOOST_CHECK(validate_config(schema, json("a")).empty());
    BOOST_CHECK(validate_config(schema, json("b")).empty());
    BOOST_CHECK(!validate_config(schema, json("d")).empty());
}

// Array items: each element is validated against "items", with the index in
// the error path.
BOOST_AUTO_TEST_CASE(array_items_are_validated) {
    json schema = {{"type", "array"}, {"items", {{"type", "integer"}}}};
    BOOST_CHECK(validate_config(schema, json::array({1, 2, 3})).empty());

    auto err = validate_config(schema, json::array({1, "x", 3}));
    BOOST_CHECK(!err.empty());
    BOOST_CHECK(err.find("items") == std::string::npos);  // path carries [i]
    BOOST_CHECK(err.find('[') != std::string::npos);
}

// Nested error paths combine base and key with a dot (join_path non-empty
// branch).
BOOST_AUTO_TEST_CASE(nested_property_error_path) {
    json schema = {{"type", "object"},
                   {"properties", {{"port", {{"type", "integer"}}}}}};
    json value = {{"port", "notanumber"}};
    auto err = validate_config(schema, value);
    BOOST_CHECK(!err.empty());
    BOOST_CHECK(err.find("port") != std::string::npos);
}

// apply_defaults recurses into nested objects that are already present.
BOOST_AUTO_TEST_CASE(apply_defaults_recurses_into_nested_objects) {
    json schema = {{"properties",
                    {{"server",
                      {{"properties",
                        {{"port", {{"default", 8080}}},
                         {"host", {{"default", "localhost"}}}}}}}}}};
    json value = json{{"server", json::object()}};
    apply_defaults(schema, value);
    BOOST_REQUIRE(value["server"].contains("port"));
    BOOST_CHECK_EQUAL(value["server"]["port"].get<int>(), 8080);
    BOOST_CHECK_EQUAL(value["server"]["host"].get<std::string>(), "localhost");

    // Non-object values are left untouched (early return).
    json scalar = 5;
    apply_defaults(schema, scalar);
    BOOST_CHECK_EQUAL(scalar.get<int>(), 5);
}

// collect_secret_paths walks nested properties and reports secret leaves as
// dot-paths; non-object schemas are ignored.
BOOST_AUTO_TEST_CASE(collect_secret_paths_walks_properties) {
    json schema = {
        {"properties",
         {{"password", {{"secret", true}, {"type", "string"}}},
          {"nested", {{"properties", {{"token", {{"secret", true}}}}}}}}}};
    std::vector<std::string> paths;
    collect_secret_paths(schema, "", paths);
    BOOST_REQUIRE_EQUAL(paths.size(), 2u);
    BOOST_CHECK(std::find(paths.begin(), paths.end(), "password") !=
                paths.end());
    BOOST_CHECK(std::find(paths.begin(), paths.end(), "nested.token") !=
                paths.end());

    // Non-object schema: no-op.
    std::vector<std::string> none;
    collect_secret_paths(json("scalar"), "", none);
    BOOST_CHECK(none.empty());
}
