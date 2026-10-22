# Base Components and Runtime Adaptation Boundaries

> Status: deferred draft. This document is not part of the current main path and is not a public API contract
> promised to business users today. The shortest current user-side mental model remains: client ingress goes
> through the gateway's single target + read-only `ClientContext`/`ClientRef`; inter-service goes through
> `shield.send/call`; external HTTP goes through `shield.http`.

What this document organizes is a more deferred advanced-capability direction: if Lua runtime primitives,
cosocket-style outbound I/O, and lower-level crypto/buffer/stream primitives are added in the future, how
should the boundaries converge.

This document's goal is not to declare these capabilities all implemented, but to prevent `auth`, business
protocol clients, or CAF details from being mixed into core again later.

## Draft Conclusion

Shield should follow the direction of Node.js and OpenResty: **core provides primitives, not business
policy**.

That is:

- Base components like `buffer`, `encoding`, `crypto`, `socket`, `tls`, `stream`, `codec`, and `url` should
  be provided.
- Upper-layer policy like JWT, OAuth, WeChat, Steam, and business RPC clients should not be made a central
  capability of core or official plugins.
- CAF does not directly reuse the Lua-facing API; CAF and the Lua runtime reuse only the lower-level pure
  primitive library.

## Layered Model

Recommended split into four layers:

```text
应用层
  -> auth / jwt / oauth / webhook / 自定义 RPC client

Lua 运行时表面
  -> shield.buffer / shield.encoding / shield.crypto
  -> shield.socket / shield.tls / shield.stream
  -> shield.codec / shield.url / shield.http

纯基础原语库
  -> bytes / endian / base64 / digest / hmac / aes-gcm / codec core / tls common

CAF / net / transport 适配器
  -> mailbox / scheduler / node transport / listener / session / gateway bridge
```

Rules:

- The Lua-facing API and the CAF adapter must not directly depend on each other's surface models.
- Both sides may only reuse the "pure base primitive library".
- Application-layer capabilities build on the Lua-facing API or plugin interfaces; they never enter
  primitives in reverse.

## Why not `auth-first`

JWT/Auth is a concrete business capability, not a base component or official plugin capability.

Problems with putting `auth` at the center of the C++ core:

- `auth` is not a primitive; it is a policy composition.
- What `jwt` really depends on is `json`, `base64url`, `hmac`, `timing_safe_equal`, clocks, and optional
  storage.
- Once `auth` becomes a core abstraction, `oauth`, `steam`, `wechat`, `firebase`, session, and ACL will all
  keep inflating along the same direction.
- That makes core increasingly look like a "built-in business framework" instead of a small-core runtime.

The cleaner direction:

- core / runtime provides `shield.crypto`
- upper-layer Lua or plugins implement `jwt/auth/webhook` on `shield.crypto + shield.codec + shield.http`

## Why Node.js and OpenResty

### What Node.js teaches

Node.js core provides:

- `Buffer`
- `crypto`
- `net`
- `tls`
- `stream`
- `http` / `https`
- `url`
- `zlib`

But Node.js core does not make JWT/OAuth/Auth built-in central capabilities. Authentication usually stays in
userland or the ecosystem layer.

### What OpenResty teaches

OpenResty's key base capability is coroutine-aware non-blocking socket primitives:

- `ngx.socket.tcp()`
- `ngx.socket.udp()`
- `sslhandshake`
- `receiveuntil`
- `setkeepalive`

Its value:

- socket I/O as a base primitive usable directly from Lua
- deeply integrated with coroutines
- letting upper layers implement Redis, MySQL, HTTP, and custom protocol clients themselves

Shield should borrow this "cosocket style" but must not copy `ngx.*` naming, request phase binding, or Nginx
semantics.

## Base Component Inventory

The inventory below represents the **target component division**. Only a few partially exist today; many
remain future work.

### P0: base components to freeze first, if this direction advances

#### `shield.buffer`

Responsibilities:

- binary buffers
- slicing/concatenation
- big/little-endian integer read/write
- byte length computation

Suggested capabilities:

- `alloc(size)`
- `from_string(s)`
- `from_hex(s)`
- `slice(offset, length)`
- `concat(list)`
- `byte_length(value)`
- `read_u16be/le`
- `read_u32be/le`
- `write_u16be/le`
- `write_u32be/le`

Notes:

- It is the common foundation of `socket`, `stream`, `crypto`, and protocol codecs.
- The current `raw` codec and the gateway raw-byte send path should both map onto this layer eventually.

#### `shield.encoding`

Responsibilities:

- text and byte encoding conversion

Suggested capabilities:

- `hex_encode/decode`
- `base64_encode/decode`
- `base64url_encode/decode`
- `url_encode/decode`
- `utf8_validate`

Notes:

- JWT, webhooks, signature strings, HTTP queries, and debug logs all need it.

#### `shield.crypto`

Responsibilities:

- stable cryptographic primitive wrappers, not an exposed OpenSSL object model

Suggested capabilities:

- `random_bytes`
- `random_uuid`
- `digest`
- `hmac`
- `timing_safe_equal`
- `hkdf`
- `pbkdf2`
- `aes_gcm_encrypt`
- `aes_gcm_decrypt`

Deferrable capabilities:

- asymmetric signing/verification
- certificate parsing
- JWKS

Notes:

- The Lua API must not be named `shield.openssl.*`
- whether OpenSSL is used underneath is an implementation detail

Implementation status (slice one landed 2026-09-28; slice two's four cuts all landed 2026-10-09, AD-08):

The slice-one capabilities implemented with RFC vector tests (API table in
lua-api.md):

- `base64_encode` / `base64_decode` / `base64url_encode` /
  `base64url_decode` / `hex_encode` / `hex_decode`
- `sha256` / `hmac_sha256`
- `random_bytes`
- `constant_time_compare`

**Naming conventions differing from the suggestions above (intentional)**: the suggested `digest` / `hmac` /
`timing_safe_equal` are algorithm-agnostic names; this round landed **algorithm-specific names** `sha256` /
`hmac_sha256` / `constant_time_compare`. Rationale: the algorithm is part of the signature;
string algorithm names like `digest(data, "sha512")` defer errors to run time (a misspelled algorithm name
yields an empty digest or throws), while specific names make "add SHA-512 / SHA-3" a **new function** rather
than semantic expansion of an existing one — old call sites keep their semantics and no extra parameters or
error branches are needed for algorithm selection. `constant_time_compare` keeps OpenSSL's existing
`CRYPTO_memcmp` name rather than `timing_safe_equal`, because its behavior (unequal lengths are simply
false; the length itself is not defended) needs to be clear from the name.

The slice-two capabilities implemented with RFC vector tests (all four cuts landed 2026-10-09, API table in
lua-api.md):

- key derivation: `pbkdf2_hmac_sha256` (RFC 8018), `hkdf_sha256` (RFC 5869)
- symmetric AEAD: `aead_aes256gcm_encrypt` / `aead_aes256gcm_decrypt`
  (12-byte nonce, 16-byte tag appended, combined form)
- asymmetric signing/verification: `ed25519_public_key` / `ed25519_sign` /
  `ed25519_verify` (RFC 8032, 64-byte deterministic signatures)
- JWT/JWKS: `scripts/lib/jwt.lua`'s `alg=EdDSA` issue/verify and
  `jwks_build` / `jwks_parse` (RFC 7517 / RFC 8037, OKP/Ed25519)

Still deferred (unimplemented): `random_uuid`, certificate parsing.

#### `shield.socket`

Responsibilities:

- Lua coroutine-aware non-blocking outbound socket primitives

Suggested capabilities:

- `tcp()`
- `udp()`
- `unix()`
- `connect()`
- `send()`
- `receive()`
- `receiveuntil()`
- `shutdown()`
- `close()`
- `settimeout()/settimeouts()`
- `setkeepalive()`
- `getreusedtimes()`

Rules:

- socket handles belong only to the current Lua VM / service
- passing sockets across services is not allowed
- multiple coroutines may not simultaneously be readers of the same socket

Notes:

- This layer should primarily serve the "outbound client" scenario first; it should not let Lua dynamically
  take over the generic listener/accept model from day one.

#### `shield.tls`

Responsibilities:

- TLS handshake and certificate verification capability

Suggested form:

- preferably as methods on `shield.socket`, e.g. `sock:tls_handshake(opts)`

Suggested capabilities:

- SNI
- certificate verification switch
- CA path/certificate configuration
- session reuse

Notes:

- A Lua abstraction fully independent of socket is not recommended.

#### `shield.stream`

Responsibilities:

- socket-oriented read/write stream semantics

Suggested capabilities:

- `read_exact(n)`
- `read_until(delim)`
- `read_frame(decoder)`
- `write(data)`
- `write_frame(encoder)`
- backpressure / buffered write

Notes:

- sockets solve "connection"; streams solve "partial packets, splitting, buffering, read/write semantics".
- Many high-level clients will build directly on `stream` rather than bare `socket`.

#### `shield.codec`

Responsibilities:

- generic data encode/decode primitives

Suggested minimal capabilities:

- `json.encode/decode`
- `msgpack.encode/decode`

Notes:

- `protobuf` / `xmldef` / `fbs` fit better as part of a protocol family or descriptor/profile system; they
  are not a set of top-level script APIs the minimal runtime core must hard-code.
- But if the underlying pure codec capability is needed, it can still attach as `shield.codec.protobuf` via
  an optional module.

#### `shield.url`

Responsibilities:

- URL parsing and query handling

Suggested capabilities:

- `parse(url)`
- `format(parts)`
- `encode_query(tbl)`
- `decode_query(str)`

Notes:

- `http`, OAuth, webhook signing, and callbacks all depend on it.

### P1: high value but deferrable

#### `shield.http`

Positioning:

- a high-level HTTP client

Notes:

- `shield.http` already exists in the repo
- long term it should be explicitly built on `socket + tls + stream + url + codec`
- it must not become the sole network entry replacing these base primitives

#### `shield.compress`

Suggested capabilities:

- `gzip`
- `deflate`
- `brotli`

#### `shield.dns`

Suggested capabilities:

- `lookup`
- `resolve`

#### `shield.websocket`

Suggested scope:

- client first
- servers stay on the fixed listener/gateway system

## What Must Not Enter the Base Component Layer

The following must not be fixed parts of runtime primitives or core:

- JWT / OAuth providers
- Steam / WeChat / Firebase login
- Redis / MySQL / MongoDB clients
- business RPC clients
- ACL / RBAC / session stores
- framework-level middleware chains

They belong to:

- the Lua application layer
- the plugin layer
- official optional modules

## Relationship to CAF

CAF cannot directly reuse the Lua-facing `socket/stream/http` APIs.

Reasons:

- the Lua socket model is coroutine I/O primitives
- the CAF model is actor mailbox / scheduler / transport
- their concurrency models, object ownership, and lifecycle semantics differ

### What CAF may reuse

What CAF / cluster / transport adapters should reuse is the pure base primitives:

- bytes / buffer helper
- encoding
- crypto
- codec core
- tls common

### What CAF must not reuse

CAF must not directly reuse these Lua surface APIs:

- `shield.socket`
- `shield.stream`
- `shield.http`
- coroutine-aware timeout / keepalive / receiveuntil semantics

The correct approach:

- the Lua runtime builds one adapter
- the CAF transport builds another adapter
- both share the same primitives

## Impact on Current Network/Gateway Semantics

This primitives design does not overturn the current gateway model.

The boundaries stay:

- inbound listeners / sessions / gateway continue to be managed by `shield_net + shield_transport`
- read-only `ClientContext`/`ClientRef` remain the fixed business handle of the network entry
- `shield.socket` mainly serves Lua-initiated outbound connections
- the first stage should not let Lua dynamically create generic server-side listeners to replace the current
  gateway/session model

That is:

- `ClientContext`/`ClientRef` faces "client connections that were admitted"
- `shield.socket` faces "outbound connections Lua proactively initiates"

They are not the same object model.

## Impact on the Plugin System

The plugin system remains responsible for:

- databases
- caches
- queues
- leaderboards
- specific auth providers (built by the business layer)

Business-layer provider design should prefer reusing base components rather than reinventing primitives.

For example:

- a JWT provider's cryptographic primitives should build on `shield.crypto`
- it should no longer be seen as a mandatory Lua runtime core capability

## Current Implementation Status

As of this document's version:

- `shield.http` exists
- the old OpenSSL frame-level encryption fragments in transport have been deleted
- `shield.crypto` has landed its first slice (see "Implementation status" above):
  codec + SHA-256 / HMAC-SHA256 + `random_bytes` +
  `constant_time_compare`
- JWT/Auth is not released as an official Shield plugin; the reference implementation moved to the Lua-side
  `scripts/lib/jwt.lua` (AD-08)

But the following are not yet formally frozen and implemented as stable base components:

- `shield.buffer`
- `shield.encoding`
- `shield.socket`
- `shield.tls`
- `shield.stream`
- a unified `shield.codec` Lua surface
- `shield.crypto` certificate parsing (the KDF / symmetric / asymmetric / JWKS four cuts
  landed 2026-10-09, see Implementation status above)

So apart from `shield.crypto` slices one and two, this document remains a **deferred layered draft** — not a
declaration of current implementation completion, and not a current public API promise.

## If Advanced Later, Suggested Order

Suggested order:

1. freeze `shield.buffer` / `shield.encoding` / `shield.crypto`
2. freeze `shield.socket` / `shield.tls` / `shield.stream`
3. explicitly refactor `shield.http` into a high-level API built on those primitives
4. make explicit that JWT/Auth is implemented by the business layer on the primitives or external services
5. add `codec`, `compress`, `websocket`, `dns` later as needed

## References

- [Node.js API index](https://nodejs.org/api/index.html)
- [Node.js Buffer](https://nodejs.org/api/buffer.html)
- [Node.js Crypto](https://nodejs.org/api/crypto.html)
- [Node.js Net](https://nodejs.org/api/net.html)
- [Node.js TLS](https://nodejs.org/api/tls.html)
- [Node.js Stream](https://nodejs.org/api/stream.html)
- [Node.js URL](https://nodejs.org/api/url.html)
- [Node.js HTTP](https://nodejs.org/api/http.html)
- [Node.js Zlib](https://nodejs.org/api/zlib.html)
- [OpenResty `ngx.socket.tcp`](https://openresty-reference.readthedocs.io/en/latest/Lua_Nginx_API/#ngxsockettcp)
- [OpenResty `ngx.socket.udp`](https://openresty-reference.readthedocs.io/en/latest/Lua_Nginx_API/#ngxsocketudp)
