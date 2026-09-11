// Coverage-focused tests for the RPC descriptor table
// (rpc_descriptor.{hpp,cpp}). The descriptor set is the single static source of
// client RPC routes: declared per actor under actors[].rpc.routes, merged at
// bootstrap, and compiled into each owning VM at spawn time.
#define BOOST_TEST_MODULE CovRpcDescriptor

#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <string>

#include "shield/transport/rpc_descriptor.hpp"

using shield::transport::PacketKind;
using shield::transport::route_entry_from_descriptor;
using shield::transport::RouteAction;
using shield::transport::RouteDirection;
using shield::transport::RpcDescriptor;
using shield::transport::RpcDescriptorTable;

namespace {

RpcDescriptor make_descriptor(std::uint32_t id, std::string name,
                              std::string binding = "do_it") {
    RpcDescriptor descriptor;
    descriptor.route_id = id;
    descriptor.name = std::move(name);
    descriptor.binding = std::move(binding);
    return descriptor;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(rpc_descriptor)

BOOST_AUTO_TEST_CASE(AddFindAndNameLookup) {
    RpcDescriptorTable table;
    BOOST_CHECK(table.add(make_descriptor(1, "login", "do_login")));
    BOOST_CHECK(table.add(make_descriptor(2, "", "anonymous")));
    BOOST_CHECK_EQUAL(table.size(), 2u);

    const auto* found = table.find(1);
    BOOST_REQUIRE(found != nullptr);
    BOOST_CHECK_EQUAL(found->name, "login");
    BOOST_CHECK_EQUAL(found->binding, "do_login");
    BOOST_CHECK(table.find(3) == nullptr);

    const auto* by_name = table.find_by_name("login");
    BOOST_REQUIRE(by_name != nullptr);
    BOOST_CHECK_EQUAL(by_name->route_id, 1u);
    BOOST_CHECK(table.find_by_name("missing") == nullptr);
    BOOST_CHECK(table.find_by_name("") == nullptr);
}

BOOST_AUTO_TEST_CASE(AddRejectsDuplicateIdAndName) {
    RpcDescriptorTable table;
    BOOST_CHECK(table.add(make_descriptor(1, "login")));
    BOOST_CHECK(!table.add(make_descriptor(1, "other")));
    BOOST_CHECK(!table.add(make_descriptor(2, "login")));
    BOOST_CHECK_EQUAL(table.size(), 1u);
}

BOOST_AUTO_TEST_CASE(MergeMergesAllAndReportsConflict) {
    RpcDescriptorTable table;
    RpcDescriptorTable other;
    BOOST_CHECK(table.add(make_descriptor(1, "login")));
    BOOST_CHECK(other.add(make_descriptor(2, "move")));
    BOOST_CHECK(other.add(make_descriptor(3, "logout")));

    std::string error;
    BOOST_CHECK(table.merge(other, &error));
    BOOST_CHECK_EQUAL(table.size(), 3u);

    RpcDescriptorTable conflicting;
    BOOST_CHECK(conflicting.add(make_descriptor(9, "same_id")));
    BOOST_CHECK(conflicting.add(make_descriptor(1, "dup")));
    BOOST_CHECK(!table.merge(conflicting, &error));
    BOOST_CHECK_NE(error.find("rpc route conflict: id 1"), std::string::npos);

    // Conflict on an unnamed descriptor mentions only the id.
    RpcDescriptorTable unnamed;
    BOOST_CHECK(unnamed.add(make_descriptor(1, "")));
    BOOST_CHECK(!table.merge(unnamed, &error));
    BOOST_CHECK_EQUAL(error, "rpc route conflict: id 1");
}

BOOST_AUTO_TEST_CASE(ClearResetsTable) {
    RpcDescriptorTable table;
    BOOST_CHECK(table.add(make_descriptor(1, "login")));
    table.clear();
    BOOST_CHECK_EQUAL(table.size(), 0u);
    BOOST_CHECK(table.find(1) == nullptr);
    // Names are cleared too: a re-add with the same name succeeds.
    BOOST_CHECK(table.add(make_descriptor(1, "login")));
}

BOOST_AUTO_TEST_CASE(RouteEntryFromDescriptorMapsTransportFields) {
    RpcDescriptor descriptor = make_descriptor(7, "chat", "do_chat");
    descriptor.direction = RouteDirection::Bidirectional;
    descriptor.requires_auth = false;
    descriptor.kind = PacketKind::Request;
    descriptor.policy.action = RouteAction::ForwardRaw;
    descriptor.policy.lazy_decode = false;
    descriptor.owner_service = "player";

    const auto entry = route_entry_from_descriptor(descriptor);
    BOOST_CHECK_EQUAL(entry.route_id, 7u);
    BOOST_CHECK_EQUAL(entry.debug_name, "chat");
    BOOST_CHECK(entry.direction == RouteDirection::Bidirectional);
    BOOST_CHECK(!entry.requires_auth);
    BOOST_CHECK(entry.kind == PacketKind::Request);
    BOOST_CHECK(entry.policy.action == RouteAction::ForwardRaw);
    BOOST_CHECK(!entry.policy.lazy_decode);
    // Lua-facing fields are intentionally not carried onto the wire path.
    BOOST_CHECK_EQUAL(entry.codec_id, 0u);
    BOOST_CHECK_EQUAL(entry.schema_id, 0u);
}

BOOST_AUTO_TEST_CASE(ParseRejectsMalformedJson) {
    RpcDescriptorTable table;
    std::string error;

    BOOST_CHECK(
        !shield::transport::parse_rpc_routes_json("{not json", table, &error));
    BOOST_CHECK_EQUAL(error, "rpc.routes is not valid JSON");

    BOOST_CHECK(!shield::transport::parse_rpc_routes_json(R"({"a": 1})", table,
                                                          &error));
    BOOST_CHECK_EQUAL(error, "rpc.routes must be a JSON array");

    BOOST_CHECK(
        !shield::transport::parse_rpc_routes_json(R"([42])", table, &error));
    BOOST_CHECK_EQUAL(error, "rpc.routes[] entries must be objects");
}

BOOST_AUTO_TEST_CASE(ParseRejectsMissingIdZeroIdAndMissingBinding) {
    RpcDescriptorTable table;
    std::string error;

    BOOST_CHECK(!shield::transport::parse_rpc_routes_json(
        R"([{"name": "noid"}])", table, &error));
    BOOST_CHECK_EQUAL(error, "rpc.routes[].id is required and must be >= 1");

    BOOST_CHECK(!shield::transport::parse_rpc_routes_json(
        R"([{"id": 0, "binding": "x"}])", table, &error));
    BOOST_CHECK_EQUAL(error, "rpc.routes[].id is required and must be >= 1");

    BOOST_CHECK(!shield::transport::parse_rpc_routes_json(R"([{"id": 1}])",
                                                          table, &error));
    BOOST_CHECK_NE(error.find("rpc.routes[].binding is required (route id 1)"),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(ParseRejectsBadDirectionAndAction) {
    RpcDescriptorTable table;
    std::string error;

    BOOST_CHECK(!shield::transport::parse_rpc_routes_json(
        R"([{"id": 1, "binding": "x", "direction": "sideways"}])", table,
        &error));
    BOOST_CHECK_EQUAL(error, "rpc.routes[].direction is invalid");

    BOOST_CHECK(!shield::transport::parse_rpc_routes_json(
        R"([{"id": 1, "binding": "x", "direction": 3}])", table, &error));
    BOOST_CHECK_EQUAL(error, "rpc.routes[].direction must be a string");

    BOOST_CHECK(!shield::transport::parse_rpc_routes_json(
        R"([{"id": 1, "binding": "x", "action": "teleport"}])", table, &error));
    BOOST_CHECK_EQUAL(error, "rpc.routes[].action is invalid");

    BOOST_CHECK(!shield::transport::parse_rpc_routes_json(
        R"([{"id": 1, "binding": "x", "action": 9}])", table, &error));
    BOOST_CHECK_EQUAL(error, "rpc.routes[].action must be a string");
}

BOOST_AUTO_TEST_CASE(ParseRejectsDuplicateIdOrName) {
    std::string error;

    RpcDescriptorTable by_id;
    BOOST_CHECK(!shield::transport::parse_rpc_routes_json(
        R"([{"id": 1, "binding": "x"}, {"id": 1, "binding": "y"}])", by_id,
        &error));
    BOOST_CHECK_NE(
        error.find("rpc.routes contains duplicate id or name (route id 1)"),
        std::string::npos);

    RpcDescriptorTable by_name;
    BOOST_CHECK(!shield::transport::parse_rpc_routes_json(
        R"([{"id": 1, "name": "dup", "binding": "x"},
            {"id": 2, "name": "dup", "binding": "y"}])",
        by_name, &error));
    BOOST_CHECK_NE(error.find("duplicate id or name"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(ParseAcceptsCanonicalContractAndDefaults) {
    RpcDescriptorTable table;
    std::string error;
    BOOST_REQUIRE(shield::transport::parse_rpc_routes_json(
        R"json([
    {"id": 1, "name": "login", "binding": "do_login",
     "action": "decode_local"},
    {"id": 2, "binding": "do_move", "direction": "client_to_server",
     "owner_service": "player", "request_codec": "json",
     "request_schema": "move.req", "response_schema": "move.resp",
     "requires_auth": false, "action": "forward_raw", "lazy_decode": false},
    {"id": 3, "binding": "push_helper", "direction": "s2c",
     "requires_auth": true},
    {"id": 4, "binding": "chat", "direction": "bidi", "action": "drop"}
  ])json",
        table, &error));

    BOOST_CHECK_EQUAL(table.size(), 4u);
    const auto* login = table.find(1);
    BOOST_REQUIRE(login != nullptr);
    // Defaults: c2s, auth required, lazy decode, Message kind, decode_local.
    BOOST_CHECK(login->direction == RouteDirection::ClientToServer);
    BOOST_CHECK(login->requires_auth);
    BOOST_CHECK(login->policy.lazy_decode);
    BOOST_CHECK(login->policy.action == RouteAction::DecodeLocal);
    BOOST_CHECK(login->kind == PacketKind::Message);

    const auto* move = table.find(2);
    BOOST_REQUIRE(move != nullptr);
    BOOST_CHECK_EQUAL(move->owner_service, "player");
    BOOST_CHECK_EQUAL(move->request_codec, "json");
    BOOST_CHECK_EQUAL(move->request_schema, "move.req");
    BOOST_CHECK_EQUAL(move->response_schema, "move.resp");
    BOOST_CHECK(!move->requires_auth);
    BOOST_CHECK(move->policy.action == RouteAction::ForwardRaw);
    BOOST_CHECK(!move->policy.lazy_decode);

    const auto* push = table.find(3);
    BOOST_REQUIRE(push != nullptr);
    BOOST_CHECK(push->direction == RouteDirection::ServerToClient);

    const auto* chat = table.find(4);
    BOOST_REQUIRE(chat != nullptr);
    BOOST_CHECK(chat->direction == RouteDirection::Bidirectional);
    BOOST_CHECK(chat->policy.action == RouteAction::Drop);
}

BOOST_AUTO_TEST_SUITE_END()
