# Deployment

Deployment guidance for the single-node-first runtime: build artifacts, containers, process management, graceful shutdown, and observability hooks.
Multi-node belongs to the official `shield_cluster` optional module (see Cluster Semantics);
this document does not cover it.

## Build Artifacts

```bash
./build.sh release          # artifact build/bin/shield (build.sh always uses the build/ directory)
./build/bin/shield --check-config --config config/app.yaml   # pre-deployment self-check
```

- The unified entry point is the `shield` executable; `--config` defaults to `config/app.yaml`,
  and multiple configs may be passed repeatedly and merged in order (CLI contract see
  [Bootstrap Runtime Semantics](runtime-bootstrap.md)).
- Always run `--check-config` before deployment: it executes the **full** bootstrap (service spawn, port
  binding) and then shuts down, exposing problems YAML validation cannot catch, such as missing
  scripts, occupied ports, and dangling bindings. Failure means refuse to deploy.
- Slow networks for dependency installs: point the vcpkg binary cache at a persistent volume so reinstalls don't recompile —
  ```bash
  export VCPKG_DEFAULT_BINARY_CACHE=/var/cache/shield/vcpkg
  ```

## Containers

The repo ships a multi-stage Dockerfile (runs `--check-config` self-check at build time, non-root at run time):

```bash
docker build -t my-shield .
docker run -p 7900:7900 my-shield
```

Port conventions (aligned with the repo's default config/app.yaml):

| Port | Purpose | Publishing policy |
|------|------|---------|
| 7900 | Game client TCP (echo entry, idlen+json) | Publish externally |
| 8080 | HTTP ops (`/ops/health` `/ops/metrics` …) | **Do not publish**; `127.0.0.1` inside the container for probes/sidecars to scrape |
| console | Unix socket `/tmp/shield-console.sock` | Never exposed over the network |

To make ops externally scrapable: explicitly change the `ops` listen address to `0.0.0.0:8080` (or use a
deployment reverse proxy) and add your own network-layer access control — the HTTP ops security stance see
Ops Runtime Semantics.

## Process Management (systemd)

SIGINT/SIGTERM uniformly trigger cooperative shutdown in-process (`shield::request_stop()` →
shutdown staged drain). Give the shutdown window enough `TimeoutStopSec`; do not shorten the kill
timeout and interrupt the drain (a half-written save is worse than waiting a few extra seconds):

```ini
# /etc/systemd/system/shield.service
[Unit]
Description=Shield game server
After=network-online.target

[Service]
User=shield
WorkingDirectory=/opt/shield
ExecStartPre=/opt/shield/bin/shield --check-config --config /opt/shield/config/app.yaml
ExecStart=/opt/shield/bin/shield --config /opt/shield/config/app.yaml
Restart=on-failure
# allow the full shutdown.timeout.total window from config/app.yaml (default 60s)
TimeoutStopSec=70

[Install]
WantedBy=multi-user.target
```

`ExecStartPre` reuses `--check-config`; with a broken config the service never even enters the startup flow.

## Graceful Shutdown Semantics

The shutdown order and budgets are declared explicitly in configuration (`shutdown.timeout`, defaults):

| Stage | Default | Meaning |
|------|------|------|
| `service_drain` | 30000ms | Services drain in-flight messages (player save flushes, etc.) |
| `service_stop` | 10000ms | Services exit |
| `plugin_shutdown` | 10000ms | Plugins shut down (connection pool drain) |
| `total` | 60000ms | Total budget; systemd `TimeoutStopSec` should exceed it |

Business-side shutdown hooks (`on_exit` / the server state machine's `on_server_state_change`) run within
these windows; enlarging a window means longer rolling-deploy time — size it by your save duration.

## Observability Hooks

- Liveness: `GET /ops/health` (read directly in-process; still answers if the actor grid is stuck).
- Metrics: `GET /ops/metrics` Prometheus 0.0.4 text; metric conventions and scrape semantics see
  Ops Runtime Semantics. A scrape interval of ≥5s is recommended.
- Diagnostics: Unix socket console (`attach` REPL, `lua.inspect`/`snapshot`/`diff`),
  see [Diagnostics Console](diagnostics-console.md); in production keep only the socket method.

## Multiple Instances

Multiple instances on one machine = multiple configs (stagger the game ports and console socket paths), organized by an external process
manager; cross-node coordination awaits `shield_cluster`. For multi-node organization before the cluster module stabilizes
(external process management / Kubernetes Service / business-layer routing) see
[Open Decisions](open-decisions.md).

## Non-Goals

- Docker Compose built-in service discovery clusters, multi-node gateway/logic/storage templates —
  those belong to `shield_cluster`.
- Prometheus scrape config templates — the metrics format is aligned; ingestion belongs to the deployer's monitoring stack.
