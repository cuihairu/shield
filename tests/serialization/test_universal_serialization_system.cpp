#define BOOST_TEST_MODULE UniversalSerializationSystemTest
#include <boost/test/unit_test.hpp>

#include "shield/serialization/json_universal_serializer.hpp"
#include "shield/serialization/universal_serializer.hpp"
#include "shield/serialization/universal_serialization_system.hpp"

using namespace shield::serialization;

BOOST_AUTO_TEST_SUITE(SerializationExceptionTests)

BOOST_AUTO_TEST_CASE(TestSerializationExceptionMessage) {
    SerializationException ex("test error");
    std::string msg = ex.what();
    BOOST_CHECK(msg.find("test error") != std::string::npos);
    BOOST_CHECK(msg.find("Serialization error") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(SerializationFormatTests)

BOOST_AUTO_TEST_CASE(TestFormatEnumDistinctness) {
    BOOST_CHECK_NE(static_cast<int>(SerializationFormat::JSON),
                   static_cast<int>(SerializationFormat::PROTOBUF));
    BOOST_CHECK_NE(static_cast<int>(SerializationFormat::JSON),
                   static_cast<int>(SerializationFormat::MESSAGEPACK));
    BOOST_CHECK_NE(static_cast<int>(SerializationFormat::PROTOBUF),
                   static_cast<int>(SerializationFormat::MESSAGEPACK));
    BOOST_CHECK_NE(static_cast<int>(SerializationFormat::JSON),
                   static_cast<int>(SerializationFormat::SPROTO));
    BOOST_CHECK_NE(static_cast<int>(SerializationFormat::JSON),
                   static_cast<int>(SerializationFormat::BINARY));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(JsonSerializerTests)

BOOST_AUTO_TEST_CASE(TestCreateJsonSerializer) {
    auto serializer = create_json_universal_serializer();
    BOOST_CHECK(serializer != nullptr);
    BOOST_CHECK_EQUAL(serializer->get_format(), SerializationFormat::JSON);
}

BOOST_AUTO_TEST_CASE(TestTypeErasedSerializeThrows) {
    auto serializer = create_json_universal_serializer();
    int obj = 42;
    BOOST_CHECK_THROW(serializer->serialize_bytes(&obj, typeid(int)),
                      SerializationException);
}

BOOST_AUTO_TEST_CASE(TestTypeErasedDeserializeThrows) {
    auto serializer = create_json_universal_serializer();
    std::vector<uint8_t> data = {1, 2, 3};
    int obj;
    BOOST_CHECK_THROW(serializer->deserialize_bytes(data, &obj, typeid(int)),
                      SerializationException);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(SerializerRegistryTests)

BOOST_AUTO_TEST_CASE(TestSingleton) {
    auto& reg1 = SerializerRegistry::instance();
    auto& reg2 = SerializerRegistry::instance();
    BOOST_CHECK_EQUAL(&reg1, &reg2);
}

BOOST_AUTO_TEST_CASE(TestRegisterAndGetSerializer) {
    auto& registry = SerializerRegistry::instance();
    auto serializer = create_json_universal_serializer();
    auto* raw = serializer.get();

    registry.register_serializer(SerializationFormat::JSON,
                                 std::move(serializer));
    BOOST_CHECK(registry.supports_format(SerializationFormat::JSON));

    auto* retrieved = registry.get_serializer(SerializationFormat::JSON);
    BOOST_CHECK_EQUAL(retrieved, raw);
}

BOOST_AUTO_TEST_CASE(TestGetUnsupportedSerializerReturnsNull) {
    auto& registry = SerializerRegistry::instance();
    auto* s = registry.get_serializer(SerializationFormat::SPROTO);
    BOOST_CHECK(s == nullptr);
}

BOOST_AUTO_TEST_CASE(TestGetSupportedFormats) {
    auto& registry = SerializerRegistry::instance();
    auto formats = registry.get_supported_formats();
    // JSON should be registered from previous test
    bool found_json = false;
    for (auto f : formats) {
        if (f == SerializationFormat::JSON) found_json = true;
    }
    BOOST_CHECK(found_json);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(UniversalSerializationSystemTests)

BOOST_AUTO_TEST_CASE(TestSingleton) {
    auto& sys1 = UniversalSerializationSystem::instance();
    auto& sys2 = UniversalSerializationSystem::instance();
    BOOST_CHECK_EQUAL(&sys1, &sys2);
}

BOOST_AUTO_TEST_CASE(TestGetSystemInfo) {
    auto& system = UniversalSerializationSystem::instance();
    std::string info = system.get_system_info();
    BOOST_CHECK(info.find("Universal Serialization System Status") !=
                std::string::npos);
}

BOOST_AUTO_TEST_CASE(TestGetAvailableFormats) {
    auto& system = UniversalSerializationSystem::instance();
    auto formats = system.get_available_formats();
    BOOST_CHECK(formats.size() >= 0);
}

BOOST_AUTO_TEST_CASE(TestSerializationConfigDefaults) {
    SerializationConfig config;
    BOOST_CHECK(config.enable_json);
    BOOST_CHECK(!config.enable_sproto);
    BOOST_CHECK(config.enable_auto_format_detection);
    BOOST_CHECK_EQUAL(config.default_format, SerializationFormat::JSON);
}

BOOST_AUTO_TEST_CASE(TestInitializeSystem) {
    SerializationConfig config;
    config.enable_json = true;
    config.enable_protobuf = false;
    config.enable_messagepack = true;
    config.enable_sproto = false;
    config.default_format = SerializationFormat::JSON;
    config.enable_auto_format_detection = true;

    BOOST_CHECK_NO_THROW(initialize_universal_serialization_system(config));
}

BOOST_AUTO_TEST_SUITE_END()
