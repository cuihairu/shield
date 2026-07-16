// [SHIELD_CORE] CAF message types for service actor transport
//
// NOTE on placement: these types are defined in `namespace shield::lua` (they
// serve the Lua service layer) but the header lives under include/shield/core/
// because the CAF adapter in shield_core must construct and send them. The
// source-boundary check forbids shield_core from including headers under
// shield/lua/, so the shared transport contract has to live one layer down.
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
// init_ready_atom: sent by the spawning thread once on_init has completed.
// Until then the service actor stashes every incoming message (see
// message-stashing in lua_service.cpp spawn) so that fork/timer/call messages
// cannot race with on_init on the same Lua VM.
CAF_ADD_ATOM(shield_lua, shield::lua, init_ready_atom)
// fork_task_atom: shield.fork routes the task to the owning service actor
// (payload = task_id, looked up in the pending_tasks map). Replaces the
// worker/pump_once drain path.
CAF_ADD_ATOM(shield_lua, shield::lua, fork_task_atom)

CAF_END_TYPE_ID_BLOCK(shield_lua)
