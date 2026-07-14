// [SHIELD_LUA] CAF message types replacing JSON-string transport
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
#include <string>

namespace shield::lua {

/// Priority mirrors the two values actually used on the CAF transport: High for
/// system/lifecycle messages, Normal for everything else. Kept separate from
/// Mailbox::Priority (which has 4 levels) to avoid pulling the Mailbox header
/// into this message-type header.
enum class MessagePriority : uint8_t {
    High = 0,
    Normal = 1,
};

/// Primary service message for send() / send_system() / send_call_request().
/// Replaces the previous default ("message") JSON kind. The coroutine-call
/// correlation fields (call_session / call_reply_to) distinguish a plain send
/// from a call request.
struct ServiceMessage {
    std::string sender;
    std::string method;
    nlohmann::json args;
    std::string trace_id;
    int64_t deadline_ms = 0;
    MessagePriority priority = MessagePriority::Normal;
    int64_t timestamp_ms = 0;
    uint64_t call_session = 0;  // non-zero => call request
    std::string call_reply_to;
};

/// Synchronous call request routed from manager->call() through the CAF actor.
/// Replaces the previous "sync_call" JSON kind. Carries sync_session so the
/// callee's dispatch can route the response back to the blocking caller.
struct SyncCallMessage {
    uint64_t sync_session = 0;
    std::string sender;
    std::string method;
    nlohmann::json args;
    std::string trace_id;
    int64_t deadline_ms = 0;
    int64_t timestamp_ms = 0;
};

}  // namespace shield::lua

// Allow the JSON-bearing types to be passed as CAF messages within a single
// actor system. CAF will not attempt to (de)serialize them; this is safe for
// local anon_send / send, which is the only transport used today.
CAF_ALLOW_UNSAFE_MESSAGE_TYPE(shield::lua::ServiceMessage)
CAF_ALLOW_UNSAFE_MESSAGE_TYPE(shield::lua::SyncCallMessage)

// -- CAF type ID block --------------------------------------------------------
//
// Assigns stable type IDs to Shield's custom CAF message types so that CAF's
// runtime type registry (used for pattern matching, introspection, and future
// serialization) recognizes them. Atoms are tag types carrying a uint64_t
// payload; structs carry full message data.
CAF_BEGIN_TYPE_ID_BLOCK(shield_lua, caf::first_custom_type_id)

// Structured message types (full payload).
CAF_ADD_TYPE_ID(shield_lua, (shield::lua::ServiceMessage))
CAF_ADD_TYPE_ID(shield_lua, (shield::lua::SyncCallMessage))

// Lightweight tag messages: atom + uint64_t payload.
// timer_fire_atom replaces kind="timer" (payload = timer_id).
// call_timeout_atom replaces kind="call_timeout" (payload = session).
CAF_ADD_ATOM(shield_lua, shield::lua, timer_fire_atom)
CAF_ADD_ATOM(shield_lua, shield::lua, call_timeout_atom)

CAF_END_TYPE_ID_BLOCK(shield_lua)
