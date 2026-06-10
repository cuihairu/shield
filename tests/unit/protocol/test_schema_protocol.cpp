#define BOOST_TEST_MODULE SchemaProtocolTest
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "shield/protocol/schema_protocol.hpp"

using namespace shield::protocol;

BOOST_AUTO_TEST_SUITE(SchemaProtocolTests)

BOOST_AUTO_TEST_CASE(TestLoadSchemaFromXmlString) {
    const std::string xml = R"xml(
<schema>
  <message name="LoginRequest" id="1001" kind="rpc" direction="c2s" timeout_ms="3000">
    <field name="username" id="1" type="string" required="true"/>
    <field name="password" id="2" type="string" required="true"/>
  </message>
  <message name="LoginReply" id="1002" kind="rpc" direction="s2c">
    <field name="token" id="1" type="string"/>
  </message>
</schema>
)xml";

    SchemaRegistry registry;
    BOOST_REQUIRE(registry.load_from_xml_string(xml));

    BOOST_CHECK_EQUAL(registry.messages().size(), 2);
    BOOST_CHECK(registry.find_message_by_id(1001) != nullptr);
    BOOST_CHECK(registry.find_message_by_name("LoginReply") != nullptr);
    BOOST_CHECK_NE(registry.schema_hash(), 0);
}

BOOST_AUTO_TEST_CASE(TestEncodeDecodeMessage) {
    MessageDefinition definition;
    definition.name = "LoginRequest";
    definition.id = 1001;
    definition.kind = MessageKind::RPC;
    definition.direction = MessageDirection::C2S;

    MessageEnvelope envelope;
    envelope.message_id = 1001;
    envelope.correlation_id = 42;
    envelope.fields.push_back(
        MessageField{1, {ProtocolValue("alice"), ProtocolValue("bob")}});
    envelope.fields.push_back(MessageField{2, {ProtocolValue("secret")}});

    auto encoded = encode_message(definition, envelope);

    SchemaRegistry registry;
    const std::string xml = R"xml(
<schema>
  <message name="LoginRequest" id="1001" kind="rpc" direction="c2s">
    <field name="username" id="1" type="string" repeated="true"/>
    <field name="password" id="2" type="string"/>
  </message>
</schema>
)xml";
    BOOST_REQUIRE(registry.load_from_xml_string(xml));

    auto decoded = decode_message(registry, encoded);
    BOOST_CHECK_EQUAL(decoded.message_id, 1001);
    BOOST_CHECK_EQUAL(decoded.correlation_id, 42);
    BOOST_CHECK_EQUAL(decoded.fields.size(), 2);
    BOOST_CHECK_EQUAL(decoded.fields[0].values.size(), 2);
    BOOST_CHECK_EQUAL(decoded.fields[0].values[0].to_string(), "alice");
    BOOST_CHECK_EQUAL(decoded.fields[1].values[0].to_string(), "secret");
}

BOOST_AUTO_TEST_CASE(TestRpcTaskSuccessAndTimeout) {
    PendingRpcRegistry registry;
    auto task = registry.create<std::string>(77, std::chrono::milliseconds(20));

    std::string ok_value;
    RpcError err_value;
    bool err_called = false;

    task->on_ok([&](const std::string& value) { ok_value = value; })
        .on_err([&](const RpcError& error) {
            err_called = true;
            err_value = error;
        });

    task->resolve("ok");
    BOOST_CHECK(task->ready());
    BOOST_CHECK_EQUAL(ok_value, "ok");
    BOOST_CHECK(!err_called);
    BOOST_CHECK(!registry.contains(77));
}

BOOST_AUTO_TEST_CASE(TestRpcTaskTimeoutPath) {
    PendingRpcRegistry registry;
    auto task = registry.create<std::string>(88, std::chrono::milliseconds(10));

    RpcError err_value;
    task->on_err([&](const RpcError& error) { err_value = error; });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    BOOST_CHECK(task->ready());
    BOOST_CHECK(err_value.code == RpcErrorCode::TIMEOUT);
}

BOOST_AUTO_TEST_SUITE_END()
