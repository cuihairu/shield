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
