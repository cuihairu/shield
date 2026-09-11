// [SHIELD_TRANSPORT] RPC descriptor: the single static source of client RPC
// routes. Descriptors are declared per actor under `actors[].rpc.routes`,
// merged into the gateway validation table at bootstrap, and compiled into
// each target VM's `route_id -> binding` table at spawn time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "shield/transport/protocol.hpp"

namespace shield::transport {

/// One client RPC method declaration. `name` is for logs/debug only and is
/// never placed on the wire; `binding` is the target Lua method name for
/// c2s/bidi routes and the generated server-to-client helper name for s2c
/// routes; `owner_service` names the actor whose VM compiles the binding
/// (empty = the declaring actor itself).
struct RpcDescriptor {
    std::uint32_t route_id = 0;
    std::string name;
    RouteDirection direction = RouteDirection::ClientToServer;
    bool requires_auth = true;
    std::string binding;
    std::string owner_service;
    std::string request_codec;  // empty = profile default codec
    std::string request_schema;
    std::string response_schema;
    RoutePolicy policy;
    PacketKind kind = PacketKind::Message;
};

/// Id-keyed table of descriptors. Rejects duplicate route ids and duplicate
/// non-empty names so conflicts fail at bootstrap instead of at dispatch.
class RpcDescriptorTable {
public:
    /// Adds `descriptor`. Returns false (and does not insert) when the route
    /// id or a non-empty name collides with an existing entry.
    bool add(const RpcDescriptor& descriptor);

    const RpcDescriptor* find(std::uint32_t route_id) const;
    const RpcDescriptor* find_by_name(std::string_view name) const;

    /// Adds every entry of `other`. On the first conflict returns false and
    /// sets `error` (when provided) describing the colliding route; entries
    /// added before the conflict remain.
    bool merge(const RpcDescriptorTable& other, std::string* error = nullptr);

    void clear();
    std::size_t size() const { return descriptors_.size(); }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const auto& [id, descriptor] : descriptors_) {
            fn(descriptor);
        }
    }

private:
    std::unordered_map<std::uint32_t, RpcDescriptor> descriptors_;
    std::unordered_map<std::string, std::uint32_t> names_;
};

/// Derives the transport-level lookup entry (used by the protocol pipeline's
/// RouteTable) from a descriptor. Lua-facing fields (binding, owner_service,
/// schemas) are intentionally not carried onto the wire-path entry.
RouteEntry route_entry_from_descriptor(const RpcDescriptor& descriptor);

/// Parses the JSON array form of `actors[].rpc.routes` into `table`. Each
/// element supports: id (required, >= 1), name, direction (c2s|s2c|bidi,
/// default c2s), requires_auth (default true), binding (required, non-empty),
/// owner_service, request_codec, request_schema, response_schema, action
/// (decode_local|forward_raw|drop), lazy_decode. Returns false with *error
/// set on malformed input or in-table conflicts.
bool parse_rpc_routes_json(std::string_view routes_json,
                           RpcDescriptorTable& table,
                           std::string* error = nullptr);

}  // namespace shield::transport
