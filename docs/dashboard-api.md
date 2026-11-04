# Dashboard HTTP API Contract (FIX-WEB-01)

This document is the authoritative contract for the read-only `/ops/*` HTTP
API that the dashboard frontend consumes. It pins, per endpoint: HTTP method,
path, request parameters, response status codes, and the JSON envelope.

The frontend (React + TS + antd + ProComponents) polls these endpoints over
the same origin as the ops HTTP listener. Every JSON endpoint shares one
envelope shape:

```json
{ "type": "result", "data": { ... } }
```

Errors use `{ "type": "error", "data": <message-string> }` unless the
endpoint documents a different shape. The HTTP status code carries the
outcome; `type`/`data` is the body envelope (so a 503 from `/ops/health`
still has a readable `data.checks`).

## Binding

The ops HTTP listener is configured by the `http:` config block. It binds
`http.host` (default `127.0.0.1`) and `http.port` (default `8080`). The
frontend is served from the same origin (see FIX-WEB-04).

## Authentication (FIX-WEB-03)

The read-only `/ops/*` endpoints are protected by a bearer-token gate. Set
`http.ops_auth_token` to a non-empty secret to enable it; an empty or absent
value leaves the gate off (legacy behaviour, no auth). When enabled, every
protected endpoint denies requests without a matching
`Authorization: Bearer <token>` header.

- **Auth**: `Authorization: Bearer <token>` (constant-time compare); when
  `http.ops_auth_token` is enabled, that gate must be satisfied first.
- **Status**: `401` on missing/mismatched token; the body is
  `{ "type": "error", "data": "unauthorized" }` and never echoes the
  configured token or any hint about it.
- **Exempt**: `GET /ops/health` is always unauthenticated. It is the k8s
  liveness/readiness probe; it only exposes process-local health state and
  never secrets, configuration, or service internals.
- **Default-deny**: when the gate is enabled, protected endpoints never
  answer without credentials, and an unauthorized request has no side
  effects (read-only endpoints never mutate state).
- **Opt-in eval/profile**: `POST /ops/eval` and `POST /ops/profile` keep
  their own independent token gates (`http.eval_token`,
  `http.profile_token`) and are additionally behind the ops-wide gate when
  it is enabled.

## Endpoints

### GET /ops/health

Lightweight process-local probe: reads happen in the HTTP thread with no
Lua actor round trip, so it answers even when the actor mesh is wedged.

- **Status**: `200` when `data.status == "ok"`; `503` when `data.status ==
  "degraded"` (body envelope unchanged).
- **Envelope**: `{ "type": "result", "data": { "status", "uptime", "checks" } }`

```json
{
  "type": "result",
  "data": {
    "status": "ok",
    "uptime": 3600.5,
    "checks": {
      "core": { "status": "ok", "uptime_seconds": 3600.5 },
      "plugins": { "status": "ok", "started": 2, "required_down": 0 }
    }
  }
}
```

`checks` is trimmed by build switches and runtime state:

- `core`: always present; `uptime_seconds`.
- `plugins`: always present; `started`, `required_down`.
- `server`: present only when the server module is built and initialized;
  `state` (running counts ok, starting/maintenance/shutdown degrade).
- `cluster`: present only when the cluster module is built and initialized;
  `nodes_online`, `nodes_down` (any offline/removed node degrades).
- `global`: present only when the global module is built and initialized;
  no counters, `status` only.

### GET /ops/status

Runtime snapshot (services, plugins, cluster/server/global conditionals).

- **Status**: `200`.
- **Envelope**: `{ "type": "result", "data": { "services", "plugins",
  ...optional blocks } }`

```json
{
  "type": "result",
  "data": {
    "services": ["gateway", "room_1"],
    "plugins": [
      { "id": "db.default", "package": "sqlite", "state": "started", "required": true }
    ],
    "cluster": {
      "node_id": "node-1",
      "node_epoch": "1",
      "nodes": [
        { "node_id": "node-2", "address": "10.0.0.2:9000", "state": "online",
          "epoch": "1", "last_heartbeat_ms": 123456, "heartbeat_age_ms": 1200 }
      ]
    },
    "server": {
      "state": "running", "uptime_seconds": 3600.5, "name": "s1",
      "watchers": 0, "shutdown_scheduled": false
    },
    "global": {
      "data": { "keys": 42 },
      "cache": { "size": 10, "hits": 100, "misses": 3, "hit_rate": 0.97 },
      "queues": { "normal": 0, "delay": 0, "priority": 0, "broadcast": 0, "reliable": 0 }
    }
  }
}
```

`services` is `["gateway", ...]`; on a 2s actor-mesh timeout it is the string
`"timeout"` (HTTP still 200). `cluster`/`server`/`global` appear only when the
corresponding module is built and initialized.

### GET /ops/metrics

Prometheus 0.0.4 text export.

- **Status**: `200`.
- **Content-Type**: `text/plain; version=0.0.4; charset=utf-8`.
- **Body**: Prometheus text format (not JSON). See
  [runtime-ops.md](zh/runtime-ops.md#指标导出) for the metric family list and
  their types/labels.

The dashboard's gateway/limits page parses this text or consumes a future
JSON endpoint per FIX-WEB-01. The metric families it must reconcile against
are `shield_gateway_connections_total{port}`,
`shield_gateway_rejections_total{port,reason}`,
`shield_gateway_active_sessions{port}`,
`shield_gateway_rate_limited_messages_total{port}`.

### GET /ops/services

Published service names.

- **Status**: `200`; `504` `{ "type": "error", "data": "timeout querying services" }`
  when the 2s actor-mesh wait expires.
- **Envelope**: `{ "type": "result", "data": ["gateway", "room_1"] }`.

### GET /ops/services/:name

Per-service read-only snapshot (registry snapshot via forked task).

- **Status**: `200`; `404` `{ "type": "error", "data": "service not found: <name>" }`;
  `504` `{ "type": "error", "data": "timeout querying service" }`.
- **Envelope**: `{ "type": "result", "data": { "name", "state", "script",
  "rpc_routes", "requests", "errors", "uptime_seconds", "timers",
  "pending_calls", "pending_tasks", "coroutines", "memory_kb" } }`.

`script` is recorded only for config-state runtime actors; spawn services may
omit it. `requests`/`errors` are per-lifecycle counters (respawn resets);
`uptime_seconds` counts from publish; `timers`/`pending_calls`/
`pending_tasks`/`coroutines` are instantaneous gauges; `memory_kb` is a
sampled `lua_gc(GCCOUNT)` value (not realtime, not process RSS).

### GET /ops/plugins

Plugin instance list.

- **Status**: `200`.
- **Envelope**: `{ "type": "result", "data": [ { "id", "package", "state",
  "required" } ] }`.

### GET /ops/config

Single-key config query; never a full snapshot, never echoes secrets.

- **Query**: `?key=<name>` (optional; absent → hint).
- **Status**: `200`.
- **Envelope**: `{ "type": "result", "data": <string|null> }`. Without `key`,
  `data` is `"Use ?key=<key> to query"`; unknown key → `null`; secret-bearing
  key family (password/passwd/secret/token/credential/api_key/apikey/
  access_key/private_key) → `"***redacted***"`.

### POST /ops/eval

Remote code entry (opt-in). Not registered unless `http.eval_enabled=true`
and `http.eval_token` is non-empty (otherwise the request 404s).

- **Auth**: `Authorization: Bearer <token>` (constant-time compare).
- **Status**: `401` on missing/mismatched token; `200` with result envelope;
  `4xx` for invalid request.
- **Body**: JSON with a `code` field executed in a restricted VM.
- **Envelope**: `{ "type": "result", "data": <lua-result> }` on success;
  `{ "type": "error", "data": <message> }` on failure.

### POST /ops/profile

Profiler control (opt-in like `/ops/eval`; requires
`http.profile_enabled=true` + non-empty `http.profile_token`).

- **Auth**: `Authorization: Bearer <token>`; when `http.ops_auth_token` is
  enabled, that gate must be satisfied first.
- **Body**: `{ "action": "status"|"start"|"stop"|"report", ... }`.
- **Status**: `200`; `404` service missing; `409` session already running;
  `429` cooldown; `401` bad token.
- **Envelope**: `{ "type": "result", "data": { ... } }` with the sample tree
  and `slow_calls` segment.

### GET / and GET /ops/dashboard

The embedded single-page dashboard (self-contained HTML). Serves
`text/html; charset=utf-8`, same bytes on both routes, fetches live data
client-side. This is the transitional artifact; the FIX-WEB-04 static build
replaces it per the dashboard plan.

## Contract rules for the frontend

- Poll `/ops/health`, `/ops/status`, `/ops/metrics` (2s baseline, configurable
  in FIX-WEB-10); do not assume a single poll is fresh enough.
- Never cache `/ops/*` responses (`cache: "no-store"`).
- Treat `type == "error"` and non-2xx status as a failure; render the
  endpoint's documented unavailable state, never a stale/empty table.
- The envelope is authoritative: unwrap `data` only after `type == "result"`.
- `memory_kb` is a sample, not realtime; do not render it as a live gauge.

## Source of truth

The implementation lives in `src/console/ops_http_handler.cpp`
(`OpsHttpHandler::register_routes` and the `handle_*` methods). This document
must be updated in the same commit as any contract-affecting change to those
handlers.
