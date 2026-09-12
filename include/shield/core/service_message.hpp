// [SHIELD_CORE] CAF message types for service actor transport
//
// NOTE on placement: these types are defined in `namespace shield::lua` (they
// serve the Lua service layer) but the header lives under include/shield/core/
// so that both shield_core (caf_initializer.cpp registers the type ID block)
// and shield_lua (the service actors sending/consuming them) can use the
// shared transport contract without shield_core depending on shield/lua/
// headers (forbidden by the source-boundary check).
// The types themselves depend only on CAF + nlohmann::json, both already linked
// by shield_core, so this does not widen shield_core's dependencies.
//
// These types replace the previous scheme where every CAF message was a
// JSON-serialized std::string dispatched by a `kind` field. Each message kind
// now has a dedicated CAF type (or atom + payload), enabling type-safe pattern
// matching in actor behaviors.
//
// nlohmann::json is kept as the `args` carrier: Lua code consumes JSON
// natively, so converting to a byte vector would just add a round-trip. The
// types are declared as CAF-allowed-unsafe-message-types so they may be passed
// between actors within a single actor system without CAF having to serialize
// the JSON internals. (Cross-node transport would require a caf::inspect
// overload that dumps/parses the JSON as a string; that is deferred until
// shield_cluster needs it.)
#pragma once

#include <caf/allowed_unsafe_message_type.hpp>
#include <caf/type_id.hpp>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace shield::lua {

/// Priority mirrors the two values actually used on the CAF transport: High for
/// system/lifecycle messages, Normal for everything else.
enum class MessagePriority : uint8_t {
    High = 0,
    Normal = 1,
};

/// Primary service message for send() / send_system() / send_call_request().
/// Replaces the previous default ("message") JSON kind. call_session (non-zero)
/// distinguishes a plain send from a call request.
struct ServiceMessage {
    std::string sender;
    std::string method;
    nlohmann::json args;
    std::string trace_id;
    int64_t deadline_ms = 0;
    MessagePriority priority = MessagePriority::Normal;
    int64_t timestamp_ms = 0;
    uint64_t call_session = 0;  // non-zero => call request
};

/// Coroutine call response routed back to the caller service actor. The caller
/// actor owns the Lua coroutine, so it must be the actor that resumes it.
struct CallResponseMessage {
    uint64_t session = 0;
    bool ok = false;
    nlohmann::json values = nlohmann::json::array();
};

/// Trusted client identity snapshot. Produced by the gateway, carried to
/// services inside client RPC messages, and shipped back on egress/control
/// messages so the gateway can re-validate the reference. When a client
/// identity travels inside an ordinary service message payload it uses the
/// JSON marker shape produced by to_json() ("__shield_client_ref": true),
/// which the Lua API layer materializes into a read-only userdata.
struct ClientContextData {
    std::string gateway_address;  // gateway actor name (the egress route back)
    uint64_t session_id = 0;
    uint32_t session_epoch = 0;
    std::string player_id;  // empty before authentication
    std::string protocol_profile_id;

    /// JSON marker encoding (see above). Round-trips through from_json().
    nlohmann::json to_json() const;

    /// Parses a JSON marker produced by to_json(); nullopt when @p json is
    /// not a client-context marker.
    static std::optional<ClientContextData> from_json(
        const nlohmann::json& json);
};

/// Server-to-client business payload. Fire-and-forget: the gateway validates
/// the reference (registry hit, session alive, epoch equal, owner player_id,
/// descriptor direction) and enqueues the payload on the session; a rejected
/// egress is dropped with a warning, never queued or retried. Exactly one of
/// message (structured codec payload, e.g. json profile) / body_bytes (raw
/// codec payload) is carried — mirroring the codec boundary of the inbound
/// DecodedBody.
struct ClientEgress {
    ClientContextData context;
    uint32_t route_id = 0;
    std::vector<uint8_t> body_bytes;
    std::optional<nlohmann::json> message;
};

/// Validated client-to-server business payload, sent by the gateway bridge to
/// the session's bound target service. The target's compiled RPC table routes
/// route_id to its handler; the handler receives (ClientContext, request)
/// where request is decoded_request when the pipeline codec produced one, else
/// the JSON-decoded body_bytes, else the raw bytes as a string.
struct ClientIngress {
    ClientContextData context;
    uint32_t route_id = 0;
    std::vector<uint8_t> body_bytes;
    std::optional<nlohmann::json> decoded_request;
};

/// Session lifecycle notification from the gateway to the bound target
/// service. This is a typed control message, not a Lua business callback.
struct ClientControlMessage {
    enum class Kind : uint8_t {
        Bound,         // binding replaced: new target attached
        Disconnected,  // session closed: current target detached
        Reconnected,   // reserved for future reconnect handling
        Unbound,       // binding replaced away from this target
    };
    Kind kind = Kind::Disconnected;
    ClientContextData context;
    std::string reason;
};

/// shield.client.bind request, routed to the gateway actor that owns the
/// session. The response is delivered through the ordinary call protocol
/// (complete_call): ok=true carries the fresh ClientRef marker, ok=false a
/// {"code": ...} error table.
struct ClientBindRequest {
    uint64_t call_session = 0;   // 0 = fire-and-forget bind (no waiter)
    std::string sender_service;  // requesting service, for error reporting
    ClientContextData context;   // session reference + expected epoch
    std::string player_id;       // trusted identity to install
    std::string target_service;  // new single target
};

/// shield.client.close request: invalidate the binding, then close the
/// socket with the given reason.
struct ClientCloseRequest {
    ClientContextData context;
    std::string reason;
};

/// Structured exit request for a service actor, sent by manager.exit /
/// shutdown_all when they run on a foreign thread. on_exit must execute on
/// the owning actor thread (every other message does), so the actor runs the
/// handler itself and quits afterwards; the exiting thread observes
/// completion by waiting for the actor instead of touching the VM directly.
struct ServiceExitRequest {
    std::string reason;
};

}  // namespace shield::lua

// Allow the JSON-bearing types to be passed as CAF messages within a single
// actor system. CAF will not attempt to (de)serialize them; this is safe for
// local anon_send / send, which is the only transport used today.
CAF_ALLOW_UNSAFE_MESSAGE_TYPE(shield::lua::ServiceMessage)
CAF_ALLOW_UNSAFE_MESSAGE_TYPE(shield::lua::CallResponseMessage)
CAF_ALLOW_UNSAFE_MESSAGE_TYPE(shield::lua::ClientEgress)
CAF_ALLOW_UNSAFE_MESSAGE_TYPE(shield::lua::ClientIngress)
CAF_ALLOW_UNSAFE_MESSAGE_TYPE(shield::lua::ClientControlMessage)
CAF_ALLOW_UNSAFE_MESSAGE_TYPE(shield::lua::ClientBindRequest)
CAF_ALLOW_UNSAFE_MESSAGE_TYPE(shield::lua::ClientCloseRequest)
CAF_ALLOW_UNSAFE_MESSAGE_TYPE(shield::lua::ServiceExitRequest)

// -- CAF type ID block --------------------------------------------------------
//
// Assigns stable type IDs to Shield's custom CAF message types so that CAF's
// runtime type registry (used for pattern matching, introspection, and future
// serialization) recognizes them. Atoms are tag types carrying a uint64_t
// payload; structs carry full message data.
CAF_BEGIN_TYPE_ID_BLOCK(shield_lua, caf::first_custom_type_id)

// Structured message types (full payload).
CAF_ADD_TYPE_ID(shield_lua, (shield::lua::ServiceMessage))
CAF_ADD_TYPE_ID(shield_lua, (shield::lua::CallResponseMessage))
CAF_ADD_TYPE_ID(shield_lua, (shield::lua::ClientEgress))
CAF_ADD_TYPE_ID(shield_lua, (shield::lua::ClientIngress))
CAF_ADD_TYPE_ID(shield_lua, (shield::lua::ClientControlMessage))
CAF_ADD_TYPE_ID(shield_lua, (shield::lua::ClientBindRequest))
CAF_ADD_TYPE_ID(shield_lua, (shield::lua::ClientCloseRequest))
CAF_ADD_TYPE_ID(shield_lua, (shield::lua::ServiceExitRequest))

// Lightweight tag messages: atom + uint64_t payload.
// timer_fire_atom replaces kind="timer" (payload = timer_id).
// call_timeout_atom replaces kind="call_timeout" (payload = session).
// NOTE: the exclusion markers below skip the macro-generated boilerplate
// (operator==/operator!=/inspect inline definitions) in line coverage.
// Those helpers are dead code in every TU that only transports the atoms;
// their runtime behavior is exercised by test_lua_api_service_messages.
CAF_ADD_ATOM(shield_lua, shield::lua, timer_fire_atom)    // GCOVR_EXCL_LINE
CAF_ADD_ATOM(shield_lua, shield::lua, call_timeout_atom)  // GCOVR_EXCL_LINE
// init_ready_atom: sent by the spawning thread once on_init has completed.
// Until then the service actor stashes every incoming message (see
// message-stashing in lua_service.cpp spawn) so that fork/timer/call messages
// cannot race with on_init on the same Lua VM.
CAF_ADD_ATOM(shield_lua, shield::lua, init_ready_atom)  // GCOVR_EXCL_LINE
// fork_task_atom: shield.fork routes the task to the owning service actor
// (payload = task_id, looked up in the pending_tasks map).
CAF_ADD_ATOM(shield_lua, shield::lua, fork_task_atom)  // GCOVR_EXCL_LINE

CAF_END_TYPE_ID_BLOCK(shield_lua)
