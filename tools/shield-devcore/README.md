# shield-devcore

IDE-agnostic TypeScript core for shield devtools: transport framing, the ops
HTTP data face, process-level sampling, and log tailing. No VS Code imports —
a future Visual Studio shell consumes the same objects over plain Node (the
VS route: consume this as an npm package and talk to the engine HTTP face;
see the root `tools/` README note).

## Surfaces

- `protocol/envelope` — the four wire framings of
  `src/transport/protocol_envelope.cpp` (`lenprefix`, `idlen`, `typed_len`,
  `delimiter`), same width/endian/`length_includes_header`/max-frame
  semantics, decoder + encoder.
- `transport/tcpClient` — reconnecting TCP client (exponential backoff +
  jitter) feeding bytes into an `EnvelopeDecoder`; read-mostly observation of
  a gateway listener.
- `sources/opsHttp` — client for the engine's ops HTTP face
  (`src/console/ops_http_handler.cpp`): `/ops/health` (unauthenticated),
  `/ops/metrics` (Prometheus text parse, includes the process gauges
  `shield_process_rss_bytes` / `shield_process_cpu_percent` /
  `shield_process_threads` / `shield_process_open_fds`), `/ops/services`,
  `/ops/services/:name`. Bearer token (`http.ops_auth_token`) supported.
- `sources/procSampler` — `/proc/<pid>` CPU/RSS/threads/fd sampling for nodes
  without `http.enabled` (fallback face; same cpu-percent convention).
- `sources/logTail` — file-tail poller for the node log file
  (`log.file.enabled=true`, `log.file.path`), parsing the `format_record()`
  line shape, with rotation/truncation recovery. Phase one of the log face;
  the socket tee push is deferred (zero engine change in this phase).
- `sources/luaServices` — the read-only "Lua script" face: merges
  `/ops/services` with per-service snapshots and resolves the recorded
  `script` path against workspace roots. Jump-to-file only; hot reload is an
  existing console capability and stays there.
- `session` — `ShieldSession`, a start/stop composition emitting
  `metrics` / `log` / `services` / `status` events.

## Node requirements

Engine side, enable the faces you want (all off by default):

```yaml
http:
  enabled: true        # ops face: /ops/health + /ops/metrics + /ops/services
  host: "127.0.0.1"
  port: 8080
  ops_auth_token: "dev-token"   # or ops_allow_unauthenticated for local-only
log:
  file:
    enabled: true      # file-tail face
    path: "logs/shield.log"
```

## Tests

```sh
pnpm install
pnpm --filter @shield/shield-devcore test
```

Covers frame decode (all four framings, split streams, oversized frames),
TCP reconnect/backoff against a live local server, log-tail append /
rotation / split lines, ops HTTP parsing (envelopes, Prometheus text, auth),
proc parse fixtures, and a composed session smoke.
