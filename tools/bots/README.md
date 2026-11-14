# tools/bots — shield gateway load tester & chaos injector

Wire-level bot clients for the shield gateway. Bots speak the gateway's own
protocol directly (idlen envelope + JSON body), mirroring what an external
game client does — no shield libraries linked.

```
wire: [route_id:2B big-endian][payload_length:2B big-endian][body JSON]
session: TCP connect -> auth entry -> login RPC (route 1) -> session bound
         to player service -> subsequent c2s routes go there
```

These defaults are the `examples/hello_world` closure (`app.yaml`); every
route id is overridable per plan. See `docs/gateway.md` for the framing
contract.

## Build

Part of the normal build (`SHIELD_BUILD_TOOLS=ON`, default on for dev
configs): target `bots`, plus static core `shield_bots_core` reused by the
acceptance smoke test `tests/acceptance/test_bots_smoke.cpp`.

## Commands

### probe — one login+move round trip

```
bots probe --target 127.0.0.1:8001 [--timeout 2000] [--player probe-cli]
```

Exit 0 = login and move both answered. Fastest way to check a listener is
alive before/after anything else.

### run — execute a load plan

```
bots run --plan plans/hall_load.yaml [--target 127.0.0.1:8001]
```

Prints per-route stats (sent / replies / timeouts / send failures) and
latency percentiles (P50/P95/P99/max) per route plus totals. Exit 0 when no
connection or login failed and every reply-carrying route got at least one
reply.

### chaos — malformed-input injection

```
bots chaos --target 127.0.0.1:8001 [--mode all] [--count 8]
```

Modes (all = all four in sequence):

| mode | what it does |
|---|---|
| `bad_frame` | truncated header / garbage bytes / lying length field, then FIN |
| `oversize` | full 65535-byte junk body (max idlen frame); promised 65535, RST at 30000 |
| `route_flood` | 200 unknown-route frames on a live session; move-before-login |
| `disconnect_flood` | rapid connect/close cycles: zero-byte, half-header, unanswered request, RST |

Closing the offending connection is the gateway working as intended. The
verdict is the **survival probe** after each injection: a normal login+move
round trip. Any probe failure prints an `ANOMALY[...]` line and the command
exits 3 — that is a gateway boundary bug; file it.

## Load plan format

Plans are **YAML** (parsed with yaml-cpp, a repo dependency). The chorus
BOTS plan was TOML; we kept the field spirit but not the format because no
TOML library ships in this repo's dependency set and a hand-rolled parser
would repeat the hand-written-JSON escaping trap. (VS-support note: a
cross-engine plan format is being evaluated in the mistlane/chorus IDE
effort; if a common format settles there, `tools/bots` can grow a converter
— not before.)

Schema (every field optional except `target` and `work_routes`):

```yaml
name: hall-load
target: "127.0.0.1:8001"     # required (CLI --target overrides)
duration_seconds: 12         # global deadline for every connection
connections: 50              # flat count; ramp below overrides when present
ramp:                        # staged connection counts by wall-clock second
  - {at_second: 0, connections: 5}
  - {at_second: 2, connections: 25}
  - {at_second: 5, connections: 50}
requests_per_second: 5       # per connection, sequential request->reply
timeout_ms: 2000             # per-request reply deadline
reconnect_max_attempts: 3    # exponential backoff 100ms..2s, capped 2s
login:
  route_id: 1
  reply_route_id: 100
  body: {player_id: bot}     # prefix; each bot appends -<index>
work_routes:                 # required, weighted sequential mix
  - route_id: 2              # per connection: send -> await reply
    weight: 9
    reply_route_id: 200
    body: {x: 10, y: 20}
  - route_id: 3              # fire-and-forget: reply_route_id omitted,
    weight: 1                 # counted as sent, no latency measured
    body: {text: ping}
```

Correlation contract: one reply frame per request frame, in wire order,
matched by route id — the gateway has no sequence numbers (see
`docs/gateway.md`). Requests on one connection are strictly sequential, so
latency samples are uncontaminated by pipelining.

## Example plans

- `plans/hall_load.yaml` — ramp to 50 bots on the hello_world closure,
  move-only mix (~12s). This is the shape used for the numbers in the task
  log.
- `plans/smoke_light.yaml` — 4 bots, 2s, for quick listener sanity checks.

## Testing

- `tests/acceptance/test_bots_smoke.cpp` boots the hello_world closure on an
  ephemeral port, probes it, runs a small load, then runs every chaos mode
  with small counts and asserts the gateway stays healthy.
- Chaos findings against a real deployment belong in `todo.md` — a clean
  run is also recorded ("未发现" when nothing was found).
