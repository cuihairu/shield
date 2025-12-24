// tests/serialization/test_serialization.cpp
#define BOOST_TEST_MODULE SerializationTests
#include <boost/test/unit_test.hpp>

#include <map>
#include <string>
#include <vector>

#include "shield/serialization/universal_serialization_system.hpp"
#include "shield/serialization/serialization_traits.hpp"

namespace shield::serialization {

// Global test fixture to initialize serialization system
struct SerializationFixture {
    SerializationFixture() {
        SerializationConfig config;
        config.enable_json = true;
        config.enable_protobuf = false;
        config.enable_messagepack = true;
        UniversalSerializationSystem::instance().initialize(config);
    }

    ~SerializationFixture() {
        // Cleanup
    }
};

BOOST_GLOBAL_FIXTURE(SerializationFixture);

// Test data structures for JSON serialization
struct TestData {
    int id;
    std::string name;
    double value;
    std::vector<int> numbers;
    std::map<std::string, int> metadata;

    bool operator==(const TestData& other) const {
        return id == other.id && name == other.name &&
               value == other.value && numbers == other.numbers &&
               metadata == other.metadata;
    }
};

// JSON serialization support for TestData
void to_json(nlohmann::json& j, const TestData& data) {
    j = nlohmann::json{
        {"id", data.id},
        {"name", data.name},
        {"value", data.value},
        {"numbers", data.numbers},
        {"metadata", data.metadata}
    };
}

void from_json(const nlohmann::json& j, TestData& data) {
    j.at("id").get_to(data.id);
    j.at("name").get_to(data.name);
    j.at("value").get_to(data.value);
    j.at("numbers").get_to(data.numbers);
    j.at("metadata").get_to(data.metadata);
}

// Concept validation tests
BOOST_AUTO_TEST_SUITE(SerializationTraitsTests)

BOOST_AUTO_TEST_CASE(test_json_serializable_concept) {
    BOOST_CHECK(JsonSerializable<TestData>);
    BOOST_CHECK(JsonSerializable<nlohmann::json>);
    BOOST_CHECK(JsonSerializable<int>);
    BOOST_CHECK(JsonSerializable<std::string>);
    BOOST_CHECK(JsonSerializable<std::vector<int>>);
}

BOOST_AUTO_TEST_CASE(test_protobuf_serializable_concept) {
    // Test that the concept works for protobuf types
    // Note: We don't have actual protobuf message types here,
    // but the concept should compile
    BOOST_CHECK(!ProtobufSerializable<TestData>);
    BOOST_CHECK(!ProtobufSerializable<int>);
}

BOOST_AUTO_TEST_CASE(test_messagepack_serializable_concept) {
    BOOST_CHECK(MessagePackSerializable<int>);
    BOOST_CHECK(MessagePackSerializable<float>);
    BOOST_CHECK(MessagePackSerializable<double>);
    BOOST_CHECK(MessagePackSerializable<std::string>);
    BOOST_CHECK(MessagePackSerializable<std::vector<int>>);
    BOOST_CHECK((MessagePackSerializable<std::map<std::string, std::string>>));
}

BOOST_AUTO_TEST_CASE(test_detect_best_format) {
    constexpr auto json_format = detect_best_format<TestData>();
    constexpr auto int_format = detect_best_format<int>();
    constexpr auto string_format = detect_best_format<std::string>();

    BOOST_CHECK(json_format == SerializationFormat::JSON);
    BOOST_CHECK(int_format == SerializationFormat::MESSAGEPACK);
    BOOST_CHECK(string_format == SerializationFormat::MESSAGEPACK);
}

BOOST_AUTO_TEST_SUITE_END()

// Serializer registry tests
BOOST_AUTO_TEST_SUITE(SerializerRegistryTests)

BOOST_AUTO_TEST_CASE(test_register_serializer) {
    auto& registry = SerializerRegistry::instance();

    // Clear any existing serializers
    // Note: This is a singleton, so we need to be careful
    auto supported = registry.get_supported_formats();
    BOOST_CHECK(!supported.empty());
}

BOOST_AUTO_TEST_CASE(test_get_serializer) {
    auto& registry = SerializerRegistry::instance();

    // Test getting JSON serializer
    auto* json_serializer = registry.get_serializer(SerializationFormat::JSON);
    BOOST_CHECK(json_serializer != nullptr);
    BOOST_CHECK(json_serializer->get_format() == SerializationFormat::JSON);

    // Test getting MessagePack serializer
    auto* mp_serializer = registry.get_serializer(SerializationFormat::MESSAGEPACK);
    BOOST_CHECK(mp_serializer != nullptr);
    BOOST_CHECK(mp_serializer->get_format() == SerializationFormat::MESSAGEPACK);
}

BOOST_AUTO_TEST_CASE(test_supports_format) {
    auto& registry = SerializerRegistry::instance();

    // Clear and initialize with known state
    BOOST_CHECK(registry.supports_format(SerializationFormat::JSON) ||
                registry.supports_format(SerializationFormat::MESSAGEPACK) ||
                registry.supports_format(SerializationFormat::PROTOBUF));
}

BOOST_AUTO_TEST_SUITE_END()

// Universal Serialization System tests
BOOST_AUTO_TEST_SUITE(UniversalSerializationSystemTests)

BOOST_AUTO_TEST_CASE(test_system_initialization) {
    auto& system = UniversalSerializationSystem::instance();

    SerializationConfig config;
    config.enable_json = true;
    config.enable_protobuf = false;
    config.enable_messagepack = true;
    config.default_format = SerializationFormat::JSON;

    system.initialize(config);

    BOOST_CHECK(system.is_initialized());

    const auto& retrieved_config = system.get_config();
    BOOST_CHECK(retrieved_config.enable_json == config.enable_json);
    BOOST_CHECK(retrieved_config.enable_protobuf == config.enable_protobuf);
    BOOST_CHECK(retrieved_config.enable_messagepack == config.enable_messagepack);
}

BOOST_AUTO_TEST_CASE(test_get_available_formats) {
    auto& system = UniversalSerializationSystem::instance();

    SerializationConfig config;
    config.enable_json = true;
    config.enable_protobuf = false;
    config.enable_messagepack = true;

    system.initialize(config);

    auto formats = system.get_available_formats();
    BOOST_CHECK(!formats.empty());
}

BOOST_AUTO_TEST_CASE(test_get_system_info) {
    auto& system = UniversalSerializationSystem::instance();

    SerializationConfig config;
    config.enable_json = true;
    config.enable_protobuf = false;
    config.enable_messagepack = true;

    system.initialize(config);

    auto info = system.get_system_info();
    BOOST_CHECK(!info.empty());
}

BOOST_AUTO_TEST_SUITE_END()

// JSON Serialization tests
BOOST_AUTO_TEST_SUITE(JsonSerializationTests)

BOOST_AUTO_TEST_CASE(test_serialize_simple_type) {
    TestData data{42, "test", 3.14, {1, 2, 3}, {{"key", 123}}};

    auto* serializer = SerializerRegistry::instance().get_serializer(
        SerializationFormat::JSON);
    BOOST_REQUIRE(serializer != nullptr);

    auto json_str = static_cast<UniversalSerializer<SerializationFormat::JSON>*>(
        serializer)->serialize(data);

    BOOST_CHECK(!json_str.empty());
    BOOST_CHECK(json_str.find("\"id\": 42") != std::string::npos);
    BOOST_CHECK(json_str.find("\"name\": \"test\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_deserialize_simple_type) {
    TestData original{42, "test", 3.14, {1, 2, 3}, {{"key", 123}}};

    auto* serializer = SerializerRegistry::instance().get_serializer(
        SerializationFormat::JSON);
    BOOST_REQUIRE(serializer != nullptr);

    auto json_str = static_cast<UniversalSerializer<SerializationFormat::JSON>*>(
        serializer)->serialize(original);

    auto deserialized = static_cast<UniversalSerializer<SerializationFormat::JSON>*>(
        serializer)->deserialize<TestData>(json_str);

    BOOST_CHECK(deserialized == original);
}

BOOST_AUTO_TEST_CASE(test_serialize_roundtrip) {
    TestData data{100, "roundtrip", 2.718, {4, 5, 6, 7}, {{"a", 1}, {"b", 2}}};

    auto* serializer = SerializerRegistry::instance().get_serializer(
        SerializationFormat::JSON);
    BOOST_REQUIRE(serializer != nullptr);

    auto serialized = static_cast<UniversalSerializer<SerializationFormat::JSON>*>(
        serializer)->serialize(data);

    auto deserialized = static_cast<UniversalSerializer<SerializationFormat::JSON>*>(
        serializer)->deserialize<TestData>(serialized);

    BOOST_CHECK_EQUAL(deserialized.id, data.id);
    BOOST_CHECK_EQUAL(deserialized.name, data.name);
    BOOST_CHECK_CLOSE(deserialized.value, data.value, 0.001);
    BOOST_CHECK(deserialized.numbers == data.numbers);
    BOOST_CHECK(deserialized.metadata == data.metadata);
}

BOOST_AUTO_TEST_CASE(test_serialize_empty_data) {
    TestData data{0, "", 0.0, {}, {}};

    auto* serializer = SerializerRegistry::instance().get_serializer(
        SerializationFormat::JSON);
    BOOST_REQUIRE(serializer != nullptr);

    auto json_str = static_cast<UniversalSerializer<SerializationFormat::JSON>*>(
        serializer)->serialize(data);

    BOOST_CHECK(!json_str.empty());
}

BOOST_AUTO_TEST_SUITE_END()

// MessagePack Serialization tests
BOOST_AUTO_TEST_SUITE(MessagePackSerializationTests)

BOOST_AUTO_TEST_CASE(test_serialize_int) {
    int value = 42;

    auto* serializer = SerializerRegistry::instance().get_serializer(
        SerializationFormat::MESSAGEPACK);
    BOOST_REQUIRE(serializer != nullptr);

    auto bytes = static_cast<UniversalSerializer<SerializationFormat::MESSAGEPACK>*>(
        serializer)->serialize(value);

    BOOST_CHECK(!bytes.empty());
}

BOOST_AUTO_TEST_CASE(test_deserialize_int) {
    int original = 12345;

    auto* serializer = SerializerRegistry::instance().get_serializer(
        SerializationFormat::MESSAGEPACK);
    BOOST_REQUIRE(serializer != nullptr);

    auto serialized = static_cast<UniversalSerializer<SerializationFormat::MESSAGEPACK>*>(
        serializer)->serialize(original);

    auto deserialized = static_cast<UniversalSerializer<SerializationFormat::MESSAGEPACK>*>(
        serializer)->deserialize<int>(serialized);

    BOOST_CHECK_EQUAL(deserialized, original);
}

BOOST_AUTO_TEST_CASE(test_serialize_string) {
    std::string str = "Hello, MessagePack!";

    auto* serializer = SerializerRegistry::instance().get_serializer(
        SerializationFormat::MESSAGEPACK);
    BOOST_REQUIRE(serializer != nullptr);

    auto bytes = static_cast<UniversalSerializer<SerializationFormat::MESSAGEPACK>*>(
        serializer)->serialize(str);

    BOOST_CHECK(!bytes.empty());
}

BOOST_AUTO_TEST_CASE(test_deserialize_string) {
    std::string original = "Deserialization test";

    auto* serializer = SerializerRegistry::instance().get_serializer(
        SerializationFormat::MESSAGEPACK);
    BOOST_REQUIRE(serializer != nullptr);

    auto serialized = static_cast<UniversalSerializer<SerializationFormat::MESSAGEPACK>*>(
        serializer)->serialize(original);

    auto deserialized = static_cast<UniversalSerializer<SerializationFormat::MESSAGEPACK>*>(
        serializer)->deserialize<std::string>(serialized);

    BOOST_CHECK_EQUAL(deserialized, original);
}

BOOST_AUTO_TEST_CASE(test_serialize_vector) {
    std::vector<int> vec = {1, 2, 3, 4, 5};

    auto* serializer = SerializerRegistry::instance().get_serializer(
        SerializationFormat::MESSAGEPACK);
    BOOST_REQUIRE(serializer != nullptr);

    auto serialized = static_cast<UniversalSerializer<SerializationFormat::MESSAGEPACK>*>(
        serializer)->serialize(vec);

    auto deserialized = static_cast<UniversalSerializer<SerializationFormat::MESSAGEPACK>*>(
        serializer)->deserialize<std::vector<int>>(serialized);

    BOOST_CHECK(deserialized == vec);
}

BOOST_AUTO_TEST_CASE(test_serialize_map) {
    std::map<std::string, int> map = {{"one", 1}, {"two", 2}, {"three", 3}};

    auto* serializer = SerializerRegistry::instance().get_serializer(
        SerializationFormat::MESSAGEPACK);
    BOOST_REQUIRE(serializer != nullptr);

    auto serialized = static_cast<UniversalSerializer<SerializationFormat::MESSAGEPACK>*>(
        serializer)->serialize(map);

    auto deserialized = static_cast<UniversalSerializer<SerializationFormat::MESSAGEPACK>*>(
        serializer)->deserialize<std::map<std::string, int>>(serialized);

    BOOST_CHECK(deserialized == map);
}

BOOST_AUTO_TEST_CASE(test_serialize_double) {
    double value = 3.14159265359;

    auto* serializer = SerializerRegistry::instance().get_serializer(
        SerializationFormat::MESSAGEPACK);
    BOOST_REQUIRE(serializer != nullptr);

    auto serialized = static_cast<UniversalSerializer<SerializationFormat::MESSAGEPACK>*>(
        serializer)->serialize(value);

    auto deserialized = static_cast<UniversalSerializer<SerializationFormat::MESSAGEPACK>*>(
        serializer)->deserialize<double>(serialized);

    BOOST_CHECK_CLOSE(deserialized, value, 0.0001);
}

BOOST_AUTO_TEST_SUITE_END()

// Format-specific serialization tests
BOOST_AUTO_TEST_SUITE(FormatSpecificSerializationTests)

BOOST_AUTO_TEST_CASE(test_serialize_as_json) {
    TestData data{1, "test", 1.0, {}, {}};

    auto json_str = serialize_as<SerializationFormat::JSON>(data);

    BOOST_CHECK(!json_str.empty());
    BOOST_CHECK(json_str.find("\"id\": 1") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_deserialize_as_json) {
    TestData original{2, "deserialize", 2.0, {1}, {{"k", 1}}};

    auto json_str = serialize_as<SerializationFormat::JSON>(original);
    auto deserialized = deserialize_as<SerializationFormat::JSON, TestData>(json_str);

    BOOST_CHECK(deserialized == original);
}

BOOST_AUTO_TEST_CASE(test_serialize_as_messagepack) {
    int value = 42;

    auto bytes = serialize_as<SerializationFormat::MESSAGEPACK>(value);

    BOOST_CHECK(!bytes.empty());
}

BOOST_AUTO_TEST_CASE(test_deserialize_as_messagepack) {
    int original = 99;

    auto bytes = serialize_as<SerializationFormat::MESSAGEPACK>(original);
    auto deserialized = deserialize_as<SerializationFormat::MESSAGEPACK, int>(bytes);

    BOOST_CHECK_EQUAL(deserialized, original);
}

BOOST_AUTO_TEST_SUITE_END()

// Exception handling tests
BOOST_AUTO_TEST_SUITE(SerializationExceptionTests)

BOOST_AUTO_TEST_CASE(test_exception_message) {
    SerializationException ex("Test error");
    std::string what = ex.what();

    BOOST_CHECK(what.find("Serialization error") != std::string::npos);
    BOOST_CHECK(what.find("Test error") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_incompatible_format_detection) {
    // Test that format detection works correctly
    constexpr auto json_format = detect_best_format<TestData>();
    constexpr auto int_format = detect_best_format<int>();

    // TestData is JSON serializable
    BOOST_CHECK(json_format == SerializationFormat::JSON);

    // int is MessagePack serializable
    BOOST_CHECK(int_format == SerializationFormat::MESSAGEPACK);
}

BOOST_AUTO_TEST_SUITE_END()

// Configuration tests
BOOST_AUTO_TEST_SUITE(SerializationConfigTests)

BOOST_AUTO_TEST_CASE(test_default_config) {
    SerializationConfig config;

    BOOST_CHECK(config.enable_json == true);
    BOOST_CHECK(config.enable_protobuf == true);
    BOOST_CHECK(config.enable_messagepack == true);
    BOOST_CHECK(config.enable_sproto == false);
    BOOST_CHECK(config.default_format == SerializationFormat::JSON);
    BOOST_CHECK(config.enable_auto_format_detection == true);
}

BOOST_AUTO_TEST_CASE(test_custom_config) {
    SerializationConfig config;
    config.enable_json = false;
    config.enable_protobuf = false;
    config.enable_messagepack = true;
    config.default_format = SerializationFormat::MESSAGEPACK;
    config.enable_auto_format_detection = false;

    BOOST_CHECK(config.enable_json == false);
    BOOST_CHECK(config.enable_protobuf == false);
    BOOST_CHECK(config.enable_messagepack == true);
    BOOST_CHECK(config.default_format == SerializationFormat::MESSAGEPACK);
    BOOST_CHECK(config.enable_auto_format_detection == false);
}

BOOST_AUTO_TEST_SUITE_END()

}  // namespace shield::serialization
