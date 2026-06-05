#define BOOST_TEST_MODULE MessagePackSerializerTest
#include <boost/test/unit_test.hpp>

#include "shield/serialization/messagepack_universal_serializer.hpp"
#include "shield/serialization/universal_serializer.hpp"

using namespace shield::serialization;

BOOST_AUTO_TEST_SUITE(MessagePackSerializerTests)

BOOST_AUTO_TEST_CASE(TestCreateSerializer) {
    auto serializer = create_messagepack_universal_serializer();
    BOOST_CHECK(serializer != nullptr);
    BOOST_CHECK_EQUAL(serializer->get_format(),
                      SerializationFormat::MESSAGEPACK);
}

BOOST_AUTO_TEST_CASE(TestTypeErasedSerializeUnregisteredType) {
    auto serializer = create_messagepack_universal_serializer();
    struct CustomType {
        int x;
    };
    CustomType obj{42};
    BOOST_CHECK_THROW(serializer->serialize_bytes(&obj, typeid(CustomType)),
                      SerializationException);
}

BOOST_AUTO_TEST_CASE(TestTypeErasedDeserializeUnregisteredType) {
    auto serializer = create_messagepack_universal_serializer();
    struct CustomType {
        int x;
    };
    std::vector<uint8_t> data = {1, 2, 3};
    CustomType obj;
    BOOST_CHECK_THROW(
        serializer->deserialize_bytes(data, &obj, typeid(CustomType)),
        SerializationException);
}

BOOST_AUTO_TEST_CASE(TestRegisterCommonTypes) {
    // Should not throw
    BOOST_CHECK_NO_THROW(register_common_messagepack_types());
}

BOOST_AUTO_TEST_CASE(TestSerializeIntAfterRegistration) {
    register_common_messagepack_types();
    auto serializer = create_messagepack_universal_serializer();

    int value = 42;
    auto data = serializer->serialize_bytes(&value, typeid(int));
    BOOST_CHECK(!data.empty());

    int result = 0;
    serializer->deserialize_bytes(data, &result, typeid(int));
    BOOST_CHECK_EQUAL(result, 42);
}

BOOST_AUTO_TEST_CASE(TestSerializeStringAfterRegistration) {
    register_common_messagepack_types();
    auto serializer = create_messagepack_universal_serializer();

    std::string value = "hello";
    auto data = serializer->serialize_bytes(&value, typeid(std::string));
    BOOST_CHECK(!data.empty());

    std::string result;
    serializer->deserialize_bytes(data, &result, typeid(std::string));
    BOOST_CHECK_EQUAL(result, "hello");
}

BOOST_AUTO_TEST_CASE(TestSerializeBoolAfterRegistration) {
    register_common_messagepack_types();
    auto serializer = create_messagepack_universal_serializer();

    bool value = true;
    auto data = serializer->serialize_bytes(&value, typeid(bool));
    BOOST_CHECK(!data.empty());

    bool result = false;
    serializer->deserialize_bytes(data, &result, typeid(bool));
    BOOST_CHECK_EQUAL(result, true);
}

BOOST_AUTO_TEST_CASE(TestSerializeDoubleAfterRegistration) {
    register_common_messagepack_types();
    auto serializer = create_messagepack_universal_serializer();

    double value = 3.14;
    auto data = serializer->serialize_bytes(&value, typeid(double));
    BOOST_CHECK(!data.empty());

    double result = 0.0;
    serializer->deserialize_bytes(data, &result, typeid(double));
    BOOST_CHECK_CLOSE(result, 3.14, 0.01);
}

BOOST_AUTO_TEST_CASE(TestSerializeVectorString) {
    register_common_messagepack_types();
    auto serializer = create_messagepack_universal_serializer();

    std::vector<std::string> value = {"hello", "world"};
    auto data =
        serializer->serialize_bytes(&value, typeid(std::vector<std::string>));
    BOOST_CHECK(!data.empty());

    std::vector<std::string> result;
    serializer->deserialize_bytes(data, &result,
                                  typeid(std::vector<std::string>));
    BOOST_CHECK_EQUAL(result.size(), 2u);
    BOOST_CHECK_EQUAL(result[0], "hello");
    BOOST_CHECK_EQUAL(result[1], "world");
}

BOOST_AUTO_TEST_SUITE_END()
