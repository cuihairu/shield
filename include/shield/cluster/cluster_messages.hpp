// [SHIELD_CLUSTER] CAF wire messages for peer handshake and heartbeat (M2).
//
// Wire contract notes:
// - Every node dials every configured peer and also publishes itself, so
//   both endpoints of a pair run the same code path. Identity adoption only
//   happens on the dialer side, when the hello_ack comes back over the
//   dialed connection: its sender is exactly the proxy this node dialed,
//   which maps 1:1 to a configured peer entry. This avoids matching peers
//   by advertised listen strings (which break with 0.0.0.0-style listens).
// - Messages cross node boundaries through CAF's BASP binary protocol, so
//   each struct carries an inspect() overload (CAF_ADD_TYPE_ID alone only
//   registers the type ID; it does not generate serialization).
#pragma once

#include <caf/type_id.hpp>
#include <cstdint>
#include <string>
#include <vector>

// The cluster block starts right after the shield_lua block; this header
// declares caf::id_block::shield_lua_last_type_id.
#include "shield/core/service_message.hpp"

namespace shield::cluster {

/// Bump on any wire-format change; peers with a different version refuse
/// to complete the handshake and stay (degradable) in Connecting.
constexpr uint32_t kClusterProtoVersion = 1;

/// Dialer -> dialed peer right after a successful remote_actor connect.
struct HelloMsg {
    std::string node_id;
    uint64_t epoch = 0;
    uint32_t proto_version = kClusterProtoVersion;
};

/// Dialed peer -> dialer; the dialer adopts the announced identity.
struct HelloAckMsg {
    std::string node_id;
    uint64_t epoch = 0;
    uint32_t proto_version = kClusterProtoVersion;
};

/// Periodic liveness proof on a dialed connection. A peer that stops
/// receiving heartbeats degrades via ClusterManager::tick() (M1).
struct HeartbeatMsg {
    std::string node_id;
    uint64_t epoch = 0;
    uint64_t seq = 0;
};

/// One published service name on the sending node (M3).
struct RouteEntry {
    std::string name;
    std::string service_id;
};

/// Full local route table snapshot, sent right after the handshake and
/// piggybacked at heartbeat cadence. Full-table (not incremental) is a
/// deliberate M3 choice: idempotent, self-healing, and propagation of a
/// shutdown (empty table) needs no retract protocol. Receivers replace the
/// whole per-node bucket.
struct RoutesMsg {
    std::string node_id;
    uint64_t epoch = 0;
    std::vector<RouteEntry> routes;
};

/// Cross-node service message envelope (M4). The caller's node wraps a
/// shield.send/call into this; the callee's transport dispatches it against
/// its local LuaServiceManager. Payloads stay JSON strings so the wire types
/// carry no Lua/nlohmann dependencies.
struct EnvelopeMsg {
    std::string source_node;  // caller's node id (reply routing)
    std::string service_id;   // callee-local target service id
    std::string method;
    std::string args_json;      // JSON array of arguments
    uint64_t call_session = 0;  // caller-side session; 0 = fire-and-forget
    int32_t timeout_ms = 0;     // caller's remaining timeout (callee slack)
};

/// Callee -> caller completion of an enveloped call (M4). ok=false carries
/// error_code/error_message; ok=true carries payload_json (the values array
/// the caller's coroutine resumes with).
struct EnvelopeReplyMsg {
    uint64_t call_session = 0;
    bool ok = true;
    std::string payload_json;
    std::string error_code;
    std::string error_message;
};

template <class Inspector>
bool inspect(Inspector& f, HelloMsg& x) {
    return f.object(x).fields(f.field("node_id", x.node_id),
                              f.field("epoch", x.epoch),
                              f.field("proto_version", x.proto_version));
}

template <class Inspector>
bool inspect(Inspector& f, HelloAckMsg& x) {
    return f.object(x).fields(f.field("node_id", x.node_id),
                              f.field("epoch", x.epoch),
                              f.field("proto_version", x.proto_version));
}

template <class Inspector>
bool inspect(Inspector& f, RouteEntry& x) {
    return f.object(x).fields(f.field("name", x.name),
                              f.field("service_id", x.service_id));
}

template <class Inspector>
bool inspect(Inspector& f, RoutesMsg& x) {
    return f.object(x).fields(f.field("node_id", x.node_id),
                              f.field("epoch", x.epoch),
                              f.field("routes", x.routes));
}

template <class Inspector>
bool inspect(Inspector& f, EnvelopeMsg& x) {
    return f.object(x).fields(f.field("source_node", x.source_node),
                              f.field("service_id", x.service_id),
                              f.field("method", x.method),
                              f.field("args_json", x.args_json),
                              f.field("call_session", x.call_session),
                              f.field("timeout_ms", x.timeout_ms));
}

template <class Inspector>
bool inspect(Inspector& f, EnvelopeReplyMsg& x) {
    return f.object(x).fields(f.field("call_session", x.call_session),
                              f.field("ok", x.ok),
                              f.field("payload_json", x.payload_json),
                              f.field("error_code", x.error_code),
                              f.field("error_message", x.error_message));
}

template <class Inspector>
bool inspect(Inspector& f, HeartbeatMsg& x) {
    return f.object(x).fields(f.field("node_id", x.node_id),
                              f.field("epoch", x.epoch), f.field("seq", x.seq));
}

}  // namespace shield::cluster

// -- CAF type ID block --------------------------------------------------------
//
// Starts right after the shield_lua block so the two custom blocks never
// overlap. Atoms below drive the transport actor's internal loops only; they
// are never sent across nodes but get stable IDs like the rest of the block.
CAF_BEGIN_TYPE_ID_BLOCK(shield_cluster,
                        caf::id_block::shield_lua_last_type_id + 1)

CAF_ADD_TYPE_ID(shield_cluster, (shield::cluster::HelloMsg))
CAF_ADD_TYPE_ID(shield_cluster, (shield::cluster::HelloAckMsg))
CAF_ADD_TYPE_ID(shield_cluster, (shield::cluster::HeartbeatMsg))
CAF_ADD_TYPE_ID(shield_cluster, (shield::cluster::RouteEntry))
CAF_ADD_TYPE_ID(shield_cluster, (shield::cluster::RoutesMsg))
CAF_ADD_TYPE_ID(shield_cluster, (shield::cluster::EnvelopeMsg))
CAF_ADD_TYPE_ID(shield_cluster, (shield::cluster::EnvelopeReplyMsg))

CAF_ADD_ATOM(shield_cluster, shield::cluster,
             connect_tick_atom)                              // GCOVR_EXCL_LINE
CAF_ADD_ATOM(shield_cluster, shield::cluster, hb_tick_atom)  // GCOVR_EXCL_LINE

CAF_END_TYPE_ID_BLOCK(shield_cluster)

namespace shield::cluster {

/// Registers the cluster message types with CAF's global meta object table.
/// MUST be called before any caf::actor_system is constructed (CAF
/// requirement) — the bootstrap does this next to initialize_caf_types().
/// Idempotent.
void init_cluster_caf_types();

}  // namespace shield::cluster
