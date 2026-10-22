# TLS (the network.tls config surface)

> Status: **implemented (v1)**. This document is the design basis for TLS-enabling the client-facing TCP listeners.
> Scope: client gateway connections carried by `TcpListener`/`TcpSession`. The
> boost::beast HTTP plane of ops/console is **outside this design** (see "Relationship to beast HTTP").
> Source: the todo.md "TLS (network.tls config surface)" candidate.

## Problem

The client gateway listener currently has plain TCP only: `TcpListener` accepts a `tcp::socket` and hands
it straight to `TcpSession`, with one read point, one write point (`async_write` / `async_read_some`) plus
close. Deployments needing transport encryption (public internet ingress, cross-DC links) have no switch
to flip.

Design inputs (all verified):

1. **The config model has no global `network` node**. All network configuration hangs off actors
   (`actors[].network.*`: tcp / max_connections / rate_limit / blocklist / protocol …), validated by
   config.cpp and parsed into `RuntimeActorConfig`.
2. `TcpSession`'s IO points are few and unique: one composed `async_write` in `do_async_write`, one
   `async_read_some` in `do_receive`, one `socket_.close` in `close()`. Changing the transport layer means
   replacing only these three touch points; session semantics (backpressure, rate limiting, read idle, the
   protocol pipeline) are transport-independent.
3. The accept path already has mature rejection semantics: blocklist → connection cap → per-IP cap, all
   "close socket + counter + log + **no session object**".
4. The vcpkg dependency graph already has a top-level `openssl`; no new dependency entry is needed.
5. The listener is a chained single-socket accept (`async_accept(socket_)` then move out and re-arm);
   the moved-out socket is reopened by the next `async_accept` — this pattern works equally well for
   "accept, handshake first, then create the session".

## Decision

### Config surface (per-actor `network.tls`)

```yaml
actors:
  gateway:
    network:
      tcp: "0.0.0.0:7000"
      protocol: { ... }
      tls:
        enabled: true                  # default false
        cert_file: "certs/server.crt"  # PEM
        key_file: "certs/server.key"   # PEM, unencrypted private key
        handshake_timeout_ms: 10000    # optional, default 10000, >= 1
```

- **No `tls` configured, or `enabled: false` → behavior is byte-for-byte identical to today** (zero
  regression): the config layer does no extra validation, the listener holds no TLS state, and the accept
  path takes the original branch.
- With `enabled: true` the config layer fails loud: `cert_file` / `key_file` must exist and be non-empty
  strings; `handshake_timeout_ms` goes through `validate_int_range(1, 600000)`.
- **A second fail-loud at the bootstrap layer**: at startup `make_tls_server_context()` actually loads the
  certificates (files readable, valid PEM, public/private key match); any failure prints a clear error and
  fails startup — **never silently falls back to plaintext**. The config layer handles types; bootstrap
  handles reality.
- `network.tcp` remains the required endpoint; TLS is an overlay, not a replacement; the combination
  "enabled but no tcp" is not supported.

### Stream abstraction (SessionStream)

`TcpSession` no longer holds a `tcp::socket` directly; it holds a `std::unique_ptr<SessionStream>`
(include/shield/net/session_stream.hpp):

- `PlainStream`: wraps `tcp::socket`, logic moved as-is (the current path);
- `TlsStream`: wraps `boost::asio::ssl::stream<tcp::socket>` with the same interface.

Both implementations bind completion handlers to the executor they store (taken from the underlying socket
at construction; the session constructor overrides it with `set_executor(strand)` to the session strand) —
the session's existing "all completion callbacks land on the strand" invariant holds; inside `TcpSession`
the only change is `socket_.async_xxx(..., bind_executor(strand_, h))` becoming
`stream_->async_xxx(..., h)`.

- The original `TcpSession(SessionId, tcp::socket, ...)` constructor signature is kept, internally wrapped
  in a `PlainStream` — all existing callers are unchanged;
- A new stream-based constructor `TcpSession(SessionId, unique_ptr<SessionStream>, ...)` serves the TLS
  accept path;
- TCP_NODELAY is applied uniformly on the lowest-level socket for both transports (TLS record framing is
  itself small-frame friendly; Nagle beneath it still causes coalescing delay).

One virtual dispatch per message is negligible against TLS record encrypt/decrypt costs; no templates are
built for it.

### Accept path and handshake semantics

**All rejection gates run before the handshake**: blocklist → connection cap → per-IP cap run first as
before (reusing the existing counters); rejected connections pay no handshake cost and are never dropped
carrying a completed handshake. With TLS on, the accept branch is:

```text
accept → blocklist/limits (unchanged)
       → tls_context_ empty: PlainStream → create session (original path)
       → otherwise: heap TlsHandshake (stream + steady_timer + shared counters),
         async_handshake(server); the listener re-arms accept in parallel
```

- **Handshake success** → count `tls_handshakes_total` → create the `TcpSession` (TlsStream)
  → exactly the same subsequent path as plaintext (callback wrapping, rate limiting, backpressure, read
  idle, protocol pipeline).
- **Handshake failure** (protocol error, invalid certificate, **a plaintext client hitting the TLS port**)
  → count `tls_handshake_failures_total` + WARN log (with `ec.message()` and the peer address) + close the
  socket, **no session object** — the same semantics as a blocklist rejection. No downgrade, no plaintext
  response.
- **Handshake timeout** (slow/malicious connections that connect but never send ClientHello) → same failure
  semantics. `handshake_timeout_ms` defaults to 10000. The timeout/completion race converges with an atomic
  "first one wins" flag; exactly one side closes the socket and counts.
- Handshake state is a per-connection heap object (holding its own shared_ptr until both sides finish);
  the listener keeps only a weak reference; `stop()` closes unfinished handshakes on the way out so the io
  drains quickly.
- **Thread count agnostic**: per-connection state is touched only by its own completion chain; counting and
  the "who arrived first" determination are all atomic — no locks needed with multiple net threads.

### Shutdown semantics (no close_notify)

Server-side shutdown closes the underlying fd directly and does not send a TLS close_notify. Introducing
close_notify would mean a "wait for the peer's close_notify" suspension path and a new timeout surface,
while the server-side proactive disconnect of game long connections is already expressed at the frame
protocol layer. Known tradeoff: the client may read a truncation error instead of a clean EOF; reconnecting
recovers.

### Plaintext/TLS exclusivity (no opportunistic TLS)

A listening port is either all plaintext or all TLS. No STARTTLS / first-byte sniffing downgrade:
"allowing downgrade" is itself an active downgrade attack surface, and the frame protocol's first byte is
indistinguishable from a TLS ClientHello — sniffing only turns the judgment into a guess.

### Certificate loading and hot reload

- **v1 = one-time load at startup**. The `ssl::context` is built by bootstrap (fail-loud) and held by the
  listener as a `shared_ptr`; certificate rotation = restart the service.
- **No v1 hot reload**, because the context is referenced by in-flight `ssl::stream`s; hot replacement needs
  an atomic `shared_ptr` swap plus lifetime management of existing connections bound to the old context,
  while certificate rotation is a low-frequency ops event that cannot justify that complexity. The path is
  already reserved by the shared_ptr shape: ops console `POST /ops/tls-reload` → make a new
  context → swap into the listener → new connections use the new context, old connections drain naturally.
- Constraints: minimum TLS 1.2 (`SSL_CTX_set_min_proto_version`), compression disabled; cipher suites use
  the OpenSSL server defaults, whitelist customization is a future knob; encrypted private keys are not
  loaded (`SSL_CTX_use_PrivateKey_file` fails outright on password-protected keys, with the error reported
  as-is).

### Relationship to beast HTTP

The ops/console HTTP (http_server.cpp, boost::beast) is the ops plane, normally reachable on localhost/an
intranet; TLS-enabling it is a separate need and beast already has the `beast::ssl_stream` shape. This
design does not touch them; v1 coverage = the `TcpListener`/`TcpSession` client plane.

### Observability

`TcpListener` gains atomic counters that enter the /ops/metrics gateway block via the existing
`ListenerRegistry` snapshot:

- `shield_gateway_tls_handshakes_total{port=...}` (successful handshakes);
- `shield_gateway_tls_handshake_failures_total{port=...}` (failures + timeouts).

For plaintext listeners both values are always 0; prom_emit_group does not output empty sample groups, so
a pure-plaintext deployment's /ops/metrics output remains byte-for-byte unchanged.

### Alternatives (rejection reasons)

- **A global `network.tls` node**: the repo has no global network node; opening a global plane just for TLS
  breaks the existing "network config hangs off actors" model.
- **STARTTLS / opportunistic TLS**: see above, an active downgrade surface.
- **BoringSSL / mbedTLS**: a second TLS stack for zero benefit; OpenSSL is already in the dependency graph.
- **Synchronous handshake**: blocks the net threads; slow-handshake clients (or malicious slow connects)
  bring down the accept loop at O(connections) — must be async + deadline.
- **Session templating (`TcpSession<Stream>`)**: the listener holds `shared_ptr<Session>`; templates would
  leak the transport type into the registry/gateway/bridge and every downstream; one virtual call per
  connection is no cost justification.

## Test Strategy

1. **Config parsing/validation** (new arms in test_cov_config.cpp): `tls` not a mapping, enabled missing
   cert/key, empty paths, handshake_timeout_ms out of range, enabled not a boolean — each hits a clear
   error message; **an actor config without tls is unchanged field by field** (the zero-regression anchor).
2. **tls_context unit** (first half of test_cov_tls.cpp): missing file → false + error message;
   bad PEM → false; cert/key mismatch → false; a valid self-signed pair → true.
3. **bootstrap fail-loud** (new arm in test_cov_bootstrap): enabled pointing at a nonexistent certificate
   file → startup fails (with the corresponding bootstrap error message).
4. **Loopback handshake** (tests/coverage/test_cov_tls.cpp, runs with every build tree): a self-signed
   certificate fixture (CN=localhost, SAN IP:127.0.0.1 / DNS:localhost, expiring 2040):
   - TLS client (verify_peer + trusting the fixture CA) → TLS listener: handshake succeeds +
     frame round-trip (on_packet receives the echo);
   - plaintext client → TLS listener: connection closed, failure counter +1, no session;
   - handshake timeout: connect without sending ClientHello + a small timeout → closed + failure counter;
   - rejection first: a blocklisted IP is rejected before the handshake, TLS counters untouched;
   - counters appear in /ops/metrics via ListenerRegistry (cov cases).
5. **Plaintext regression**: existing test_tcp_listener / test_cov_listener / gateway end-to-end all green
   is the anchor (they ARE the zero-regression criterion).
6. **Gate**: ctest all green across the five trees build / build-dbg / build-plug / build-net / build-cov +
   gcovr (line 98 / branch 100 / function 100, `../src/`).

## Milestones

- **M1 (this task, landed)**: config surface + context fail-loud + SessionStream +
  the accept TLS branch + handshake counters + /ops/metrics export + tests 1–5 above.
- **M2 (unscheduled)**: ops console certificate hot reload (atomic context swap),
  TLS on the beast HTTP plane, cipher suite whitelist, client certificate verification (mTLS).
