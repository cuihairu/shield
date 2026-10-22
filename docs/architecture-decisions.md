# Architecture Decision Records

This document freezes Shield's current-phase architecture decisions as the alignment baseline for architecture design and implementation progress. Once a decision is written here, subsequent implementations must converge around that contract.

If this document conflicts with the Architecture Overview, [Core Design Principles](architecture-core-concepts.md), or the [Skynet Comparison](skynet-comparison.md), this document prevails.

---

## Background: Design Goals

Shield's design covenant is:

> **Draw on skynet's service/call/send/coroutine semantics, use CAF as the actor runtime, and make Service the only addressable RPC unit.**

See [Core Design Principles §3](architecture-core-concepts.md), [Skynet Comparison §CAF and Skynet Semantics](skynet-comparison.md).

The decisions frozen here intend to close off the CAF actor runtime, the client interaction contract, and the player hosting model, preventing any later introduction of a separate service runtime or an extra addressable object model.

---

## AD-01: CAF is the only actor runtime

**Decision**: Services must be built on CAF actor semantics; `spawn / send / call / exit` converge into
`shield_core`, and no separate runtime is built at the Lua layer.

**Rationale**: this was the project's original covenant and the core of "don't rebuild skynet". CAF provides
actor / mailbox / scheduler / request-reply / remote actors — fully sufficient; the selection was not wrong.

**Progression order** (see the roadmap):
1. [`dispatch_stack` made thread-local + a shared_mutex on the service registry (zero semantic change).]
2. [`shield.call` uses CAF request/reply to drive coroutine yield/resume. `manager->call()` routes to a CAF
   actor + condition-variable blocking wait; self-calls return an error.]
3. per-service execution rights + a global activation queue + N workers preemptive scheduling (achieving
   skynet-style multicore concurrency, serial per VM).

---

## AD-02: Service is the only addressable RPC unit

**Decision**:

- Only Services own mailboxes, CAF addresses, and `send / call` capability.
- One online player = one PlayerService = one CAF Actor = one private Lua VM.
- Player business state is that service's private Lua table; no separate runtime objects are introduced.

**Rationale**: strictly keep the skynet model — the target of `send / call` is always a Service, not an
Entity, not a player object. This keeps the owner boundary clearest, the concurrency model simplest, and
debugging easiest.

**Implications**:

- Cross-service calls: `shield.send(service, method, ...)` / `shield.call(service, method, ...)`.
- A client RPC's `route_id` first selects the logical service name via the descriptor; the Gateway then
  resolves the actual Service actor from that session's dynamic routing context. The logical service name
  defaults to `player`, but can also be `scene`, `room`, `map`, or whichever service the player is currently
  bound to.
- The target Service adapter reaches the startup-registered handler directly inside its VM by the header
  `route_id`. Business Lua never receives a generic client message and dispatches it itself.

---

## AD-03: no Entity runtime is introduced

**Decision**: the first phase does **not** do any of the following:

- `PlayerEntity` / `AvatarEntity`
- Entity mailboxes
- Entity `send / call` / Entity RPC
- a globally writable `EntityManager.get(id)`
- `AvatarShard` / `player_pool` shard hosting

Ordinary data (rooms, combat, monsters, drops, etc.) lives as Lua tables inside their owning service; only
objects that genuinely need independent actor semantics become standalone services.

**Rationale**:

- Introducing a "send/call-able PlayerEntity" stacks a second actor runtime on top of the Service actor
  runtime — complexity explodes, and owner/mailbox/timer/coroutine/RPC each get two homes.
- KBEngine's Entity can RPC because its Entity is itself an "actor-like object with domain semantics" backed
  by a full runtime; that is another route and not adopted in this phase.
- The first version prioritizes getting service/call/runtime semantics right and stable, reducing
  comprehension cost and error surface.

**Future**: if "Entity as lightweight actor" is genuinely needed, introduce it as an **explicit architecture
upgrade**, with no pre-embedding and no pollution of the current API.

See the Entities and Components draft (that document is downgraded to draft / not the
current contract).

**Clarification** (since shield_player P0): what this document forbids is the second runtime of "send/call-able
Entity actors". `shield_player`'s `PlayerSession` (module-held session metadata) and `PlayerRef` (a
`{uid, node_id, service_id, epoch}` value reference) are not actors: no mailbox, no RPC/timer/coroutine of
their own, unusable as `shield.send/call` targets — therefore outside the prohibition; `player_pool` shards
remain an optional strategy pending stress tests (OD-012).

---

## AD-04: one-player-one-service is the default capacity model

**Decision**: by default each online player corresponds to one PlayerService (an independent Lua VM). No
shard / pool optimization is pre-built for unmeasured memory pressure.

**Rationale**:

- This is the default shape of the skynet agent model: the owner is clearest, and reconnect/logout/save/
  timers all hang in one place.
- The memory tradeoff was assessed as acceptable: each player VM's real cost must be measured with real
  stress tests, not assumed and pre-optimized.
- Premature shards front-load complexity like "player routing, owner validation, in-shard state management",
  violating the "get the architecture right first" principle.

**Future**: only when real stress tests prove per-player VM cost unacceptable should
`player_pool / avatar_shard` be introduced, and as a separate architecture upgrade item that does not change
the public API.

---

## AD-05: client RPC routing design

**Decision**: the wire header carries `route_id` and the body is pure business data. All client messages go
through the Gateway to the session-bound target service (before login → AuthService, after login →
PlayerService). `route_id`'s only duty is to hit the startup-cached Lua handler inside the target Service's
VM. The Gateway does not parse the target service from `route_id` and does no multi-target routing.

**Wire format**:

```text
+------------------+------------------+
|     Header       |       Body       |
| (route_id, ...)  | (业务数据)        |
+------------------+------------------+
```

The Header carries `route_id` and other transport fields. The Body is pure business data; its encoding is
decided by the ProtocolProfile (json / protobuf / msgpack). route_id never appears in the body.

**Inbound path**:

```text
socket bytes
  → frame decode → 读 header.route_id
  → Gateway 路由表校验（route_id 合法 + direction + 认证要求）
  → session.target（AuthService 或 PlayerService）
  → CAF send ClientIngress { session_id, epoch, player_id, route_id, body_bytes }
  → 目标 VM: route_id → cached handler → decode body → invoke handler(ctx, client, request)
```

- The Gateway only reads the header's `route_id` for legality validation and does not decode the body.
- `body_bytes` pass through to the target VM as-is, decoded there per the RPC schema.
- The handler signature is fixed as `handler(ctx, ClientContext, request)` (`ctx` is the sender context
  table) and contains no route_id or raw frame.

**Pre-login routing**: before authentication the session is not yet bound to a PlayerService; pre-login RPCs
(login, token verification, etc.) target the AuthService using the same route_id → handler mechanism. After
successful auth, the Gateway atomically switches session.target = PlayerService.

**Outbound path**: codegen-generated server-to-client helpers are bound to route_id; outbound encode
addresses by the target s2c route's own schema type name (same-name convention or that route's
`request_schema` override; a reply on the same route_id is rejected by the outbound direction validation).
Business passes only ClientContext + business parameters. After the helper encodes body_bytes, CAF sends
ClientEgress to the Gateway, which writes route_id into the wire header with body_bytes as the body.

**Codec is route-independent**: the codec is fixed per session (`src/transport/protocol.cpp` resolves the
vtable at listener build time into `codecs_`), never selected per route. `codec_for_route()` ignores the
route and always returns default_codec_id (`src/transport/protocol.cpp:1520-1524`).

**RPC descriptors**: each client RPC is defined at compile time by a descriptor with route_id, name,
direction, binding, request_schema, request_codec (`schema_id` / `response_schema` / `binding_hint` removed;
their presence in config is an error; schema addressing converges to "route-name same-name convention +
`request_schema` explicit override"). The target Service compiles them at startup into a
`route_id → cached Lua handler` mapping. name is for logging and debugging only; it never enters the wire.

**Request, response, and push boundaries**:

- Shield provides descriptor-driven client RPC method dispatch, plus generated server-to-client response/push
  helpers.
- The first version provides no generic `client.call`, seq pending map, future, or automatic timeout
  correlation. If an RPC needs a correlation token, that RPC's contract defines the business field
  explicitly.
- `PacketKind` / `seq` are not exposed as a Lua API, and CAF request ids are not borrowed to build a client
  pending table.

Details in Client RPC Routing Design, the [Gateway Design](gateway.md), and the
Lua API.

---

## AD-06: SessionHandle boundary

**Decision** (implementation conventions in the successor note below; the `SessionHandle` type is deleted):

- Bare session/connection handles exist only at the network / gateway boundary; they never enter business Lua
  or CAF payloads.
- After login, every `ClientIngress` must carry the runtime-injected trusted client context: Gateway address,
  `session_id`, session epoch, `player_id`, and the protocol profile id. These are serializable values, not
  connection objects.
- Lua handlers see only the read-only `ClientContext` and that RPC's business parameters; `ClientContext` can
  derive a `ClientRef` for replies; that value internally retains the protocol profile identity but exposes no
  socket, CAF handle, codec, or frame to business.
- Player/Scene/Room/Map Services send `ClientEgress` through registered server-to-client RPC helpers; they do
  not call a generic session send API and never touch codecs / frames / the ProtocolPipeline.

**Rationale**: network connection ownership belongs to the Gateway, but any target Service directly receiving
client RPCs must know the trusted player identity and be able to send responses back to the original
connection. So a stable value-semantic client reference crosses actors, not a connection handle. This way:

- `SessionHandle` is never serialized with messages or propagated across threads.
- A target Service, local or remote, always has the trusted `player_id` and the reply reference.
- Business Services are fully decoupled from specific protocols and socket implementations.
- Owner/epoch validation for disconnects, reconnects, and stale replies concentrates in the Gateway alone.

**Successor implementation** (since the architecture correction M2): the `SessionHandle` type is deleted. The
boundaries above are carried by the read-only `ClientContext` (a per-ingress userdata) and `ClientRef` (a
reply-capable value reference, serialized across services via the `__shield_client_ref` JSON marker and
materialized as read-only userdata); the session's single-target binding and epoch are managed by the
Gateway's `SessionBinding`, and bare connection handles no longer have any business-side type.

---

## AD-07: controllable business clock (layered time)

**Decision**: time is layered — Lua business time (`shield.now` / `os.time` no-arg / `os.date` no-arg) goes
through an injectable wall-clock `Clock`, settable in tests; C++ scheduling/network time (`deadline_ms` /
`timestamp` / CAF `delayed_send` / pump scans) is locked to the real monotonic clock. `os.clock` is not wired
in (real CPU time, for performance timing). Timer **firing** keeps the real clock (scheduling layer); the
time business code reads inside callbacks is settable.

**Rationale**: game business depends heavily on date semantics (resets/events/expiry), which must be settable
to test; but network heartbeats, call timeouts, and timer firing are the scheduling layer — setting those
would break the runtime itself (connections timing out spuriously, scheduling chaos). Layering keeps
"testable" and "runtime correctness" from conflicting. Business code uses `os.time` heavily (the repo shows
~12 sites like cache expiry), so only setting `shield.now` would be pointless — the no-arg `os.time`/`os.date`
must join the same Clock. `os.clock` is left alone — the old DI/IoC experiment scripts (deleted) used it for
busy-waiting `while os.clock()-start<0.01`, and setting it would hang them.

**Implementation status**: implemented. `shield.now()` reads the `Clock` held by `LuaServiceManager`
(default `SystemClock`, `system_clock` wall-clock UTC ms); `shield.monotonic()` reads `steady_clock` (not
settable). The no-arg `os.time()`/`os.date()` hooks are installed in `register_full_shield_api`; the
parameterized forms keep the original functions. `attach_clock()` injects a `MockClock` for tests.
`Clock`/`SystemClock`/`MockClock` are defined in `include/shield/lua/clock.hpp`. `test_lua_api_clock` covers
12 cases: default wall clock, consistency, MockClock set/advance, os hook granularity (table-argument forms
unaffected), monotonic non-settability, and complete business scenarios for cache expiry/check-in/cooldown.

---

## AD-08: cryptography provides only primitives; auth semantics live in the Lua business layer

**Decision**: the runtime provides a **cryptographic primitive library** (`shield.crypto`:
base64/base64url/hex codecs, SHA-256, HMAC-SHA256, secure random bytes, constant-time comparison) and
**provides no auth/signature business semantics**. Decisions like JWT's "which claims to issue, when things
expire, which issuers are accepted" belong to business and live in the Lua layer (`scripts/lib/jwt.lua` is
the reference implementation). The former C++ plugin `plugins/auth_jwt` is deprecated accordingly and will be
removed after a transition period.

**Rationale**: auth policy is business semantics that changes with product shape (multi-tenant issuers,
refresh tokens, asymmetric signing, key rotation), while C++ plugins are an ABI-frozen binary boundary —
stuffing each policy into the plugin layer writes business decisions into the ABI. Primitives are the stable
surface: algorithms and encoding formats change slowly, are independently testable (RFC vectors), and the
implementation can directly use an existing dependency (OpenSSL is already in the dependency tree) with no
in-house development. After layering, "switching auth schemes" means changing Lua, not shipping a new binary;
the runtime also need not maintain plugin API versions for every token shape.

**Implementation status** (landed 2026-09-28, extended 2026-10-09): `include/shield/lua/lua_crypto.hpp` +
`src/lua/lua_crypto.cpp`, assembled by `register_full_shield_api`; `shield_lua` explicitly links
`OpenSSL::Crypto`. The API table is in lua-api.md. `scripts/lib/jwt.lua` (dual HS256 and EdDSA
algorithms + an RFC 7517 JWKS public key set helper) is a pure-Lua reference implementation assembled only
from primitives, with its own minimal JSON codec; the slice-two primitives (PBKDF2/HKDF, AES-256-GCM,
Ed25519) landed 2026-10-09. `plugins/auth_jwt/manifest.yaml` is marked `deprecated` (not deleted, for
existing deployments).

---

## Relationship of Decisions to Other Documents in This Repo

| Decision | Affected documents |
|---|---|
| AD-01 | architecture.md, architecture-core-concepts.md, skynet-comparison.md, runtime-messaging.md, roadmap.md |
| AD-02 | runtime-service.md, runtime-player.md, skynet-comparison.md |
| AD-03 | runtime-entity.md, runtime-player.md, architecture.md |
| AD-04 | runtime-player.md |
| AD-05 | gateway.md, protocol-routing-design.md, lua-api.md, runtime-messaging.md |
| AD-06 | runtime-network.md, runtime-player.md, gateway.md, lua-api.md |
| AD-07 | runtime-timer.md, lua-api.md |
| AD-08 | lua-api.md, plugin-system.md, architecture.md |
