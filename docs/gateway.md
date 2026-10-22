# Gateway Design

The Gateway is the boundary of client connections, session management, and the wire entry point; it is not a generic message dispatcher for business Lua.

## Responsibilities

- One Gateway actor per TCP listener, holding a live session weak-handle registry
  (`GatewaySessionRegistry`), managing connections, disconnects, kicks, and write-back.
- Maintaining the single source of truth for session binding (the Session object): target (AuthService or
  PlayerService), player_id, epoch, protocol profile; the registry keeps only connection-time snapshots
  for diagnostics.
- Parsing only the fields needed for frame/header; ordinary client RPC reads only the header `route_id`. When a listener configures a codec plugin (`network.protocol.body.provider`), the pipeline decodes the body into a canonical JSON message; the Gateway itself does no business decoding — it only forwards the raw bytes together with the decode result.
- Validating route_id legality with a lightweight routing table (existence, direction, auth requirement), rejecting illegal messages.
- After validation, delivering `ClientIngress` to session.target's CAF actor.
- Receiving the target actor's `ClientEgress`, validating session_id/epoch, writing the wire header, and writing the socket.

The Gateway holds no business handlers, does no multi-target routing, and knows nothing of room/scene/map.

## Session Binding

```text
Session {
  target           ServiceHandle    AuthService 或 PlayerService
  player_id        string?          认证后有值
  session_epoch    uint32           每次绑定更新递增
  protocol_profile string           编解码配置标识
}
```

- Before login: target = AuthService, player_id = nil.
- After login: target = PlayerService, player_id = the authentication result.
- After successful auth, `shield.client.bind` triggers the Gateway's CAS atomic update
  (`apply_binding`: compare expected epoch → write target/player_id → increment epoch);
  the bind reply returns a `ClientRef` carrying the new epoch to the calling coroutine.
- Ingress/egress/control references carrying an old epoch must be rejected or dropped.
- Dynamic routing of room/scene/map is managed by PlayerService private state and does not pass through the Gateway.

## Gateway Route Table

The Gateway maintains a lightweight validation table, built at compile time from the RPC descriptor set:

```text
route_id → { direction, requires_auth }
```

- It only validates legality: whether the route_id exists, whether the direction allows client initiation, whether auth is required.
- It contains no logical_service_name, handler, schema, or ServiceAddress.
- Messages failing validation are rejected at the Gateway and never enter the target VM.

## Inbound

```text
client socket bytes
  → frame decode → 读 header.route_id
  → Gateway 路由表校验（合法 + direction + 认证要求）
  → session.target（AuthService 或 PlayerService）
  → CAF send ClientIngress { gateway_address, session_id, session_epoch, player_id,
                              protocol_profile_id, route_id, body_bytes, decoded_request? }
  → target Service actor mailbox
  → 目标 VM: route_id → cached handler → decode body（或直接消费 decoded_request）→ invoke handler(ctx, client, request)
```

The route_id comes from the wire header. body_bytes always pass through to the target VM as-is; the
request value takes shape per the descriptor contract: when the listener's protocol pipeline has a codec
plugin configured and decoding completed, the decoded canonical JSON message rides along as
`ClientIngress.decoded_request` and serves directly as the handler's request table; otherwise routes whose
request_codec is empty or `json` have the target VM decode body_bytes as JSON (falling back to a raw byte
string on failure); other request_codecs such as `raw` always pass the raw byte string.

`ClientIngress` is a runtime-internal message, not a Lua API and not a client body schema. CAF delivers
this internal message to local or remote actors.

## Outbound

```text
target Service actor
  → shield.client_rpc.<name> helper（spawn 期按 s2c descriptor 注册，绑定 route_id）
  → CAF send ClientEgress { context, route_id, body_bytes | message } 到 gateway_address
  → Gateway 校验门（按序）：
      1. registry 命中（unknown session）
      2. epoch 相等（stale epoch）
      3. owner player_id 相等（owner mismatch）
      4. descriptor 路由存在（route not found）
      5. 方向允许 server-to-client（direction rejected）
      6. session->send_message（队列满/已关闭 = send failed）
  → route_id 写入 wire header，body_bytes 作为 body
  → frame encode → socket write
```

The outbound route_id comes from the helper's bound server-to-client descriptor; business code may also
pass structured messages (encoded by the listener's codec plugin). The Gateway does not scan business
arguments.

Rejection is warn + atomic counters (`GatewayStats`); no queuing, no retry, no error frame written back —
egress is fire-and-forget. Write-back validation uses `session_id`, the Gateway address, and the epoch,
preventing a disconnected, reconnected, or stale actor message from writing to a new connection.

## Kick and Reconnect Timing Semantics

This section makes the epoch CAS timing details explicit: what happens in what order, and the fate of
in-flight messages in each time window. The player module's kick policy (`single` / `kick_old` / `multi`,
reconnect window) is player-layer business, see runtime-player.md;
this section only describes the mechanisms and guarantees the Gateway provides.

### The exact semantics of the epoch CAS

```text
apply_binding(target, player_id, expected_epoch, &out):
    lock(binding_mutex_)
    expected_epoch != binding.epoch 且 != kAnyEpoch → 拒绝（false）
    写 target / player_id → ++epoch → 出参返回新绑定
```

- `session.hpp`'s `kAnyEpoch = 0xFFFFFFFF` is the unconditional-replacement sentinel, used only for
  **invalidation** (the close path); a normal bind always goes through the CAS with the expected epoch.
- The epoch starts at 0 (`reset_binding` on connection, target = the auth entry service); it increments on
  every successful bind, so after login (auth → player) it is at least 1.
- The bind CAS happens only on the Gateway actor (`handle_client_bind`); the mutex guarantees only one
  winner among concurrent binds; losers receive `client_rpc.epoch_expired` (warn + a
  `binds_epoch_expired` counter; with a call_session the call is completed and the failure returned).

### Three checkpoints, one non-check

| Path | epoch validation | Timing details |
|------|-----------|---------|
| bind (ingress control) | **CAS** (expected) | the winner flips dispatch: `Bound` is sent to the new target only after the registry update completes |
| egress (s2c write-back) | **equality** | `client_rpc` carrying an old ClientRef is always dropped (warn + `egress_stale_epoch`), fire-and-forget, no error frame |
| close/kick | unconditional (kAnyEpoch) | invalidate → remove → close socket |
| ingress (c2s forwarding) | **not validated** | the bridge snapshots the **live binding** on the net thread before forwarding; the context the target actor receives carries the epoch at snapshot time |

Ingress non-validation is deliberate: a c2s message is already in the pipe; dropping it would only create
silent loss. The real protection is on the **reply side** — ingress snapshotted before the bind flip
executes normally in the target VM, but if its handler sends s2c with the old ClientRef, the egress gate
rejects it on epoch equality. That is: **old-epoch processing may finish computing, but old-epoch
write-back never reaches the wire**.

### Kick ordering guarantees

The fixed order of `shield.client.close` (`handle_client_close`):

1. First send `Unbound` to the **current** target (carrying the last epoch before invalidation) — this
   must happen before invalidation, because the bridge's disconnect callback only notifies targets that
   "still have a binding"; kick must compensate for this itself, or the business never gets the offline
   notification.
2. `apply_binding("", "", kAnyEpoch)` invalidates the binding — from this moment all old-epoch references
   fail, **before the socket actually closes**.
3. Registry removal.
4. `session->close(reason)`; an empty reason is recorded as `"kicked"`.

The dual guarantee: the bridge's `on_disconnect` seeing an empty binding knows the Gateway already sent
Unbound and does not additionally send `Disconnected` (preventing double notification). **Only non-kick
natural disconnects** go through `Disconnected` (the `on_disconnect` callback, sent by the bridge).

### Reconnect

- A new TCP connection = a new session_id, a new Session, epoch from 0. The Gateway does no cross-connection
  player merging — **two live sessions for the same uid is a legal state at the Gateway layer**;
  per-player uniqueness is adjudicated by the player module (`single` rejects / `kick_old` calls
  `shield.client.close` on the old session / re-binds the reused instance within the reconnect window).
- Reconnect reusing a PlayerService instance: the new session's bind (auth → player) goes through the same
  CAS; the old session is already disconnected and invalidated, so there is no epoch race. In-flight egress
  left on the old PlayerService is dropped by the egress gate on epoch inequality.
- The `Reconnected` control message is a placeholder; currently reconnect-recovery determination happens
  inside the player module by uid state and does not depend on that message.

### In-Flight Window Quick Reference

| Event sequence | Result |
|---------|------|
| a rebind happens while a handler runs; the handler replies with the old ClientRef | egress rejects (stale epoch), warn + counter; the client never receives that frame |
| bind winner and loser race at the same instant | CAS has a single winner; the loser gets `client_rpc.epoch_expired`, and business may refresh its ClientRef and retry |
| kick vs natural disconnect race | whoever arrives first completes the Unbound notification + invalidation; the other skips at the bridge due to the empty binding, with no double notification |
| after rebind, the old target's in-flight ingress continues executing | allowed (computation not interrupted), but any of its s2c is rejected by the egress gate |

## Lua and Service Boundary

- RPC handler functions and parameters are registered at startup by RPC-definition/generated bindings.
- A handler receives only the decoded business parameters of that RPC; player identity comes from the
  trusted client context injected by the runtime.
- One online player corresponds by default to one PlayerService / CAF actor / private Lua VM.
- After all client messages enter the PlayerService, it decides internally by route_id whether to handle
  them itself or forward to room/scene/map.
- `client_disconnect` is a control notification from the Gateway to the current target, not a business RPC
  routing entry; `shield.client.close` goes through `ClientCloseRequest`: invalidate the binding first
  (epoch increment), then remove the registration, finally close the socket (an empty reason is recorded
  as "kicked").

## Non-Goals

- Business re-dispatch by route/body inside Gateway Lua.
- Gateway multi-target routing (a session multi-service binding table, `bind_service`/`unbind_service`).
- Passing Session/connection handles across services (business only gets read-only `ClientContext`/`ClientRef`).
- Letting PlayerService operate sockets, codecs, frames, or envelopes directly.
- A framework-level client RPC pending table with automatic response/future/timeout correlation.
- Using `PacketKind` or seq as the Lua client RPC API.
