# Network Runtime Semantics

This document freezes the contracts of client connections, protocol handling, and CAF actor interaction. CAF / `shield_core` is the only actor runtime boundary for client RPC ingress, egress, and service message delivery; this document preserves no legacy interfaces or compatibility flows.

## Layer Responsibilities

| Layer | Responsibility | Explicitly NOT responsible for |
| --- | --- | --- |
| `shield_net` | listeners, connection lifecycle, I/O, backpressure, live session ownership | business RPC dispatch, player state |
| `shield_transport` | frame/envelope, header, session-level codec/profile | guessing routes from the body, choosing the business actor |
| Gateway runtime | session registry, trusted identity, single-target binding, inbound/outbound validation | decoding ordinary RPC bodies, holding Lua handlers |
| CAF / `shield_core` | actor mailboxes, scheduling, local/remote message delivery | automatic mapping of client wire methods to Lua functions |
| Service adapter | client RPC binding, decode inside the target actor, Lua handler invocation | sockets, listeners, session ownership |

The CAF middleman carries Service actors' cross-process transport. A client connection always belongs to the Gateway that admitted it; whether the target business Service is local or remote does not change the upper-layer semantics.

## ProtocolProfile

Each session binds one `ProtocolProfile` at listener configuration or first-phase handshake; it cannot switch during normal business.

A profile determines at least:

- the frame/envelope format;
- the width and byte order of `route_id` in the header;
- the compiled RPC descriptor;
- the body codec and schema package;
- frame size, decode error, and unknown route policies.

Normal client RPC must carry `route_id` in the header. The body contains only that RPC's business parameters — no route name, `route_id`, method, or service name.

## Session binding: single target

The Gateway maintains exactly one current target binding per live session (`SessionBinding`,
landed in M2):

```text
SessionBinding {
  target_service
  player_id?
  gateway_name
  protocol_profile_id
  epoch
}
```

Rules:

- `session_id` locates the connection only within the current Gateway; `epoch` distinguishes owners across
  disconnect/reconnect (`SessionBinding.epoch`, see `include/shield/net/session.hpp`; the CAF message
  `ClientContextData` and the Lua-side `ClientContext`/`ClientRef` attributes are still named
  `session_epoch`).
- `player_id` is written from the authentication result; it is trusted identity and is never read from the
  client's business body.
- Before login the target is the auth entry service; after successful auth the Gateway actor atomically
  replaces target and `player_id` with a compare-and-set on the epoch (see
  protocol-routing-design.md).
- Binding replacement increments the epoch. Ingress, replies, and binding updates carrying an old epoch
  must be rejected or dropped.
- Dynamic routing of rooms, scenes, and maps is PlayerService private business state and never enters the
  Gateway; sessions have no multi-service binding table, and the `bind_service`/`unbind_service` APIs have
  been deleted.

## Inbound

```text
socket bytes
  -> shield_net
  -> frame/envelope decode
  -> read header.route_id
  -> descriptor validation table (route exists, direction, requires_auth)
  -> session single-target binding
  -> CAF send ClientIngress
  -> target Service actor mailbox
  -> route_id -> startup-compiled Lua binding
  -> decode body by profile body codec
  -> invoke handler(ctx, ClientContext, request)
```

The Gateway must not decode ordinary business bodies before choosing the target actor. Only then can the
header `route_id` serve fast forwarding, and the Gateway avoids depending on every business schema.

Pre-login RPCs declare `requires_auth: false` in the descriptor. After authentication completes, the auth
service atomically switches the target to the player service via `shield.client.bind`.

## CAF Internal Messages

Inbound uses structured runtime messages, not ordinary Lua service methods:

```text
ClientContextData {
  gateway_address
  session_id
  session_epoch
  player_id?
  protocol_profile_id
}

ClientIngress {
  ClientContextData context
  route_id
  body_codec_name
  body_bytes
  decoded_request?      // canonical JSON already decoded by the profile codec (if any)
}
```

`route_id` is internal dispatch metadata copied from the wire header, and `body_bytes` are still the raw
RPC body. Neither is re-wrapped into the client body, and neither is handed to Lua as business parameters.
The full CAF message contracts (`ClientEgress`, `ClientControlMessage`, bind/close requests) are in
runtime-messaging.md.

The CAF behavior distinguishes at least:

- `ClientIngress`;
- `ClientEgress`;
- ordinary Service `send/call`;
- lifecycle controls such as client disconnect/reconnect;
- Service lifecycle.

CAF handles message-type dispatch and actor scheduling; the Shield Service adapter handles
`route_id -> cached Lua handler`. No hand-written CAF C++ message type per RPC.

## ClientContext and ClientRef

The session connection object is Gateway-internal state; it never enters business Lua and is not propagated
through CAF messages. Business sees only read-only `ClientContext` / `ClientRef` userdata (landed in M2):
neither exposes sockets, codecs, frames, routes, CAF handles, or underlying session pointers; the read-only
attributes are `player_id` / `session_id` / `session_epoch` / `protocol_profile_id` / `gateway`;
`ClientContext` additionally provides `ref()` deriving a `ClientRef` value.

The `ClientRef` encapsulation (landed in M2):

```text
gateway_address + session_id + session_epoch + player_id + protocol_profile_id
```

It is passed as an ordinary service message parameter: on serialization it becomes the
`__shield_client_ref` JSON marker, and is re-materialized as userdata on arrival at the peer VM. The
Gateway revalidates the epoch when handling replies, closes, or binding updates, so old `ClientRef`s never
hit a new connection.

## Outbound

Services must send responses or pushes through the `shield.client_rpc.<binding>` helpers auto-registered
at spawn time per s2c descriptor (landed in M2):

```text
shield.client_rpc.<name>(ClientContext|ClientRef, business_table_or_bytes)
  -> helper supplies route_id（descriptor 绑定）
  -> table 参数编码为结构化 message，string 参数原样作为 body bytes
  -> CAF send ClientEgress to gateway_address
  -> Gateway 校验门：registry 命中 → epoch 相等 → owner player_id
     → descriptor 路由存在 → 方向允许 s2c → send_message
  -> write route_id into wire header
  -> frame/envelope encode
  -> socket write
```

There is no business send API accepting a route string, a bare `route_id`, or a generic table envelope.
Route metadata comes from the helper's bound descriptor; fields named `route`, `method`, or `id` in the
business parameters do not affect dispatch. Egress is fire-and-forget: rejection is only warn + counters;
no queuing, no retry, no error frame written back.

## Single-target binding

The auth service uses `shield.client.bind(client, player_id, target)` (landed in M2) to ask the Gateway
actor to atomically replace the current session's target binding and write `player_id`:

- bind suspends the calling coroutine; when the Gateway CAS succeeds (`apply_binding`: expected epoch
  comparison → write → epoch increment) it resumes with `(true, ClientRef)`; on failure it resumes with
  `(false, {code="client_rpc.epoch_expired"})`;
- old `ClientRef`s are invalidated by the epoch increment; clients cannot specify a target or modify
  bindings through the body;
- closing the current client uses `shield.client.close(ref, reason)`: invalidate the binding first, remove
  the registration, finally close the socket (an empty reason is recorded as "kicked");
- dynamic collaboration like room/scene/map is the target service's private state, done through ordinary
  `shield.send/call`, never through the Gateway.

## Disconnect and Reconnect

When a connection drops, the Gateway:

1. invalidates the live session and increments the epoch;
2. sends the structured `ClientControlMessage::Disconnected` control message to the current target;
3. removes the session's registry record;
4. rejects subsequent write-backs from old `ClientRef`s.

After a reconnect authenticates successfully, the Gateway creates a new epoch, and the player owner decides
whether to restore the original PlayerService or create a new instance; after restoration the target binding
is re-established. Lifecycle control messages are not client RPCs and never enter the generic business
message entry.

## Backpressure and Limits

The network layer must enforce explicit limits:

| Parameter | Description |
| --- | --- |
| `max_connections` | listener maximum connections |
| `max_connections_per_ip` | maximum connections per IP |
| `max_frame_size` | maximum bytes per frame |
| `max_session_send_queue` | per-session pending-write queue cap |
| `max_decode_errors` | not implemented — no such config key and no "consecutive protocol errors hit the cap → disconnect" counting logic |
| `read_idle_timeout` | read idle timeout (ms, `0` disables; on timeout the session closes with the `read_idle_timeout` error code) |
| `write_idle_timeout` | not implemented — no write idle timeout, only `read_idle_timeout` |
| `handshake_timeout` | no standalone transport-layer key; the only implementation is `network.tls.handshake_timeout_ms` (default `10000`, range `1-600000`), see "TLS (network.tls)" below |
| `rate_limit` | per-connection inbound token bucket (`messages_per_second` + `burst`); over-limit frames dropped without disconnect |
| `blocklist.deny` | reject by address/CIDR at accept; rejected peers get no session |

`ClientEgress` being accepted only means it entered the Gateway's write-back flow, not that the client has
received it. Full queues, stale sessions, and unreachable Gateways return clear errors; the runtime does no
unbounded caching or implicit retries.

## Threads and Execution Rights

- I/O threads execute only sockets, frame/envelope, and header processing; they never execute Lua.
- The target Service actor executes handler binding, body decode, and Lua calls at its own mailbox
  scheduling points.
- Only one executor may be inside a given Service actor / Lua VM at a time.
- Different Service actors are scheduled in parallel by the CAF scheduler.
- Frame order on one connection is preserved on the session strand; no global ordering across connections.


## Transport Scope

TCP is the required first-version transport. Every TCP listener must bind `network.protocol` (enforced by
config validation): declaring `network.tcp` without `network.protocol` fails startup. Inbound bytes on a
session without a protocol pipeline no longer have a fallback path (the old bare frame decoding/encryption
layer is deleted); the connection closes immediately with the stable error code
`protocol_not_configured`. UDP, KCP, and WebSocket may become later transport adapters, but they must reuse
the same session single-target binding, header `route_id`, `ClientIngress/ClientEgress`, and epoch
validation semantics; they must not invent different Lua client APIs for different transports.

## TLS (network.tls)

Client-facing listeners support per-actor TLS (config schema, handshake, and hot-reload tradeoffs in the
[TLS Design](tls-design.md)):

- `network.tls.enabled` (default false) + `cert_file` / `key_file` (PEM) +
  `handshake_timeout_ms` (default 10000, range 1–600000). Unconfigured or disabled, the listener behaves
  exactly as before;
- enabled is fail-loud: the config layer validates field shapes, and the bootstrap layer actually loads the
  certificates (missing files, bad PEM, public/private mismatch all fail startup), never silently falling
  back to plaintext;
- a port is either all TLS or all plaintext; no downgrade/sniffing; connections failing or timing out the
  TLS handshake are closed directly and produce no session;
- `blocklist` and connection limits still take effect before the handshake; rejected peers pay no handshake
  cost;
- minimum protocol TLS 1.2, compression disabled; server-side proactive close sends no TLS close_notify
  (disconnect semantics are expressed at the frame protocol layer);
- observability: the /ops/metrics gateway block exports
  `shield_gateway_tls_handshakes_total` and
  `shield_gateway_tls_handshake_failures_total` (with the `port` label; pure-plaintext deployments emit
  neither family).
