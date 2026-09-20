// Tests for the minimal JSON-Schema subset validator.
#define BOOST_TEST_MODULE shield_plugin_schema
#include <boost/test/included/unit_test.hpp>
#include <nlohmann/json.hpp>

#include "schema_validator.hpp"

using namespace shield::plugin;
using json = nlohmann::json;

BOOST_AUTO_TEST_CASE(type_mismatch_fails) {
    json schema = {
        {"type", "object"},
        {"properties",
         {{"port", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}}}},
        {"required", json::array({"port"})}};
    json val = {{"port", "notanumber"}};
    BOOST_CHECK(!validate_config(schema, val).empty());
}

BOOST_AUTO_TEST_CASE(required_missing_fails) {
    json schema = {{"type", "object"}, {"required", json::array({"host"})}};
    json val = json::object();
    BOOST_CHECK(!validate_config(schema, val).empty());
}

BOOST_AUTO_TEST_CASE(range_violation_fails) {
    json schema = {
        {"type", "object"},
        {"properties",
         {{"port", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}}}}}};
    json val = {{"port", 99}};
    BOOST_CHECK(!validate_config(schema, val).empty());
}

BOOST_AUTO_TEST_CASE(valid_passes) {
    json schema = {
        {"type", "object"},
        {"properties", {{"port", {{"type", "integer"}, {"default", 3306}}}}}};
    json val = {{"port", 3306}};
    BOOST_CHECK(validate_config(schema, val).empty());
}

BOOST_AUTO_TEST_CASE(apply_defaults_fills_missing) {
    json schema = {{"type", "object"},
                   {"properties", {{"port", {{"default", 3306}}}}}};
    json val = json::object();
    apply_defaults(schema, val);
    BOOST_CHECK(val.contains("port"));
    BOOST_CHECK_EQUAL(val["port"].get<int>(), 3306);
}

BOOST_AUTO_TEST_CASE(enum_violation_fails) {
    json schema = {{"type", "string"}, {"enum", json::array({"a", "b", "c"})}};
    json val = "d";
    BOOST_CHECK(!validate_config(schema, val).empty());
}

// Payload-shape strictness: with additionalProperties=false a field-name
// typo ("user_id" vs "userid") is rejected at the boundary instead of
// surfacing later inside the Lua handler.
BOOST_AUTO_TEST_CASE(payload_typo_rejected_in_strict_mode) {
    json schema = {{"type", "object"},
                   {"required", json::array({"userid"})},
                   {"properties", {{"userid", {{"type", "string"}}}}},
                   {"additionalProperties", false}};
    json good = {{"userid", "123"}};
    BOOST_CHECK(validate_config(schema, good).empty());

    json typo_only = {{"user_id", 123}};
    BOOST_CHECK(!validate_config(schema, typo_only).empty());

    json typo_extra = {{"userid", "123"}, {"user_id", 123}};
    auto err = validate_config(schema, typo_extra);
    BOOST_CHECK(err.find("user_id") != std::string::npos);
    BOOST_CHECK(err.find("additional property") != std::string::npos);
}

// Payload-schemas may use the length keywords to pin message shape.
BOOST_AUTO_TEST_CASE(length_bounds_apply_to_payload_strings) {
    json schema = {{"type", "string"}, {"minLength", 1}, {"maxLength", 32}};
    BOOST_CHECK(validate_config(schema, json("x")).empty());
    BOOST_CHECK(!validate_config(schema, json("")).empty());
    BOOST_CHECK(!validate_config(schema, json(std::string(33, 'x'))).empty());
}
