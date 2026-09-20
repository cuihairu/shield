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

// Round-3: object "required" array enforcement and the "maximum" bound for
// floating point values.
BOOST_AUTO_TEST_CASE(object_required_fields_enforced) {
    json schema = {{"type", "object"},
                   {"required", json::array({"host", "port"})}};
    BOOST_CHECK(!validate_config(schema, json{{"host", "x"}}).empty());
    BOOST_CHECK(
        validate_config(schema, json{{"host", "x"}, {"port", 1}}).empty());
}

BOOST_AUTO_TEST_CASE(float_above_maximum_fails) {
    json schema = {{"type", "number"}, {"maximum", 10.0}};
    BOOST_CHECK(!validate_config(schema, json(10.5)).empty());
    BOOST_CHECK(validate_config(schema, json(9.5)).empty());
}

// ---------------------------------------------------------------------------
// additionalProperties: boolean false = strict mode. This is the catch for
// the motivating payload typo (a "userid" vs "user_id" field name drift used
// to pass silently because unknown keys were never rejected).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(additional_properties_strict_rejects_unknown_keys) {
    json schema = {
        {"type", "object"},
        {"required", json::array({"userid"})},
        {"properties", {{"userid", {{"type", "string"}, {"minLength", 1}}}}},
        {"additionalProperties", false}};

    // Exact key match passes.
    BOOST_CHECK(validate_config(schema, json{{"userid", "123"}}).empty());

    // Typo-only payload: "required" fires first with a clear message.
    auto err = validate_config(schema, json{{"user_id", 123}});
    BOOST_CHECK_EQUAL(err, "userid: required field missing");

    // Required key present plus an unknown key: strict mode rejects it with
    // the key in the path.
    err = validate_config(schema, json{{"userid", "123"}, {"user_id", 123}});
    BOOST_CHECK_EQUAL(err, "user_id: additional property not allowed");
}

BOOST_AUTO_TEST_CASE(additional_properties_lenient_by_default) {
    json schema = {{"type", "object"},
                   {"properties", {{"a", {{"type", "integer"}}}}}};
    // Absent keyword: unknown keys pass.
    BOOST_CHECK(
        validate_config(schema, json{{"a", 1}, {"extra", true}}).empty());
    // Explicit true: lenient.
    json lenient = schema;
    lenient["additionalProperties"] = true;
    BOOST_CHECK(validate_config(lenient, json{{"a", 1}, {"extra", 1}}).empty());
    // Non-bool value: leniently ignored.
    json weird = schema;
    weird["additionalProperties"] = "nope";
    BOOST_CHECK(validate_config(weird, json{{"a", 1}, {"extra", 1}}).empty());
}

BOOST_AUTO_TEST_CASE(additional_properties_recursive_into_nested_objects) {
    json schema = {{"type", "object"},
                   {"properties",
                    {{"nested",
                      {{"type", "object"},
                       {"properties", {{"ok", {{"type", "boolean"}}}}},
                       {"additionalProperties", false}}}}},
                   {"additionalProperties", false}};

    BOOST_CHECK(
        validate_config(schema, json{{"nested", {{"ok", true}}}}).empty());
    auto err =
        validate_config(schema, json{{"nested", {{"ok", true}, {"spy", 1}}}});
    BOOST_CHECK_EQUAL(err, "nested.spy: additional property not allowed");
}

// minLength / maxLength: bounds are inclusive; messages follow the
// ": below minimum" / ": above maximum" style.
BOOST_AUTO_TEST_CASE(string_length_bounds) {
    json schema = {{"type", "string"}, {"minLength", 2}, {"maxLength", 4}};
    BOOST_CHECK(!validate_config(schema, json("x")).empty());
    BOOST_CHECK(validate_config(schema, json("xx")).empty());
    BOOST_CHECK(validate_config(schema, json("xxxx")).empty());
    BOOST_CHECK(!validate_config(schema, json("xxxxx")).empty());

    auto err = validate_config(schema, json("x"));
    BOOST_CHECK_EQUAL(err, ": below minLength");
    BOOST_CHECK_EQUAL(validate_config(schema, json("xxxxx")),
                      ": above maxLength");
}

// minItems / maxItems: applied before per-element "items" validation, so a
// too-short array reports the length, not an element error.
BOOST_AUTO_TEST_CASE(array_item_count_bounds) {
    json schema = {{"type", "array"},
                   {"minItems", 2},
                   {"maxItems", 3},
                   {"items", {{"type", "integer"}}}};
    BOOST_CHECK(!validate_config(schema, json::array({1})).empty());
    BOOST_CHECK(validate_config(schema, json::array({1, 2})).empty());
    BOOST_CHECK(validate_config(schema, json::array({1, 2, 3})).empty());
    BOOST_CHECK(!validate_config(schema, json::array({1, 2, 3, 4})).empty());
    // Elements still validated when the count is fine.
    BOOST_CHECK(!validate_config(schema, json::array({1, "x"})).empty());
    BOOST_CHECK_EQUAL(validate_config(schema, json::array({1})),
                      ": below minItems");
    BOOST_CHECK_EQUAL(validate_config(schema, json::array({1, 2, 3, 4})),
                      ": above maxItems");
}

// Non-numeric length keywords are leniently ignored (payload schemas are
// user-supplied; a malformed bound must not throw out of the validator).
BOOST_AUTO_TEST_CASE(non_numeric_length_bounds_ignored) {
    json schema = {{"type", "string"}, {"minLength", "abc"}};
    BOOST_CHECK(validate_config(schema, json("any length")).empty());
    json arr = {{"type", "array"}, {"maxItems", "lots"}};
    BOOST_CHECK(validate_config(arr, json::array({1, 2, 3, 4, 5})).empty());
}
