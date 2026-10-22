# Architecture Review

Assessment criteria: this project's goal is a **batteries-included, single-node-first game server runtime**
(skynet-inspired, Lua-first, C++ infrastructure). Each judgment is therefore measured not by "is the design
elegant" but by **"is it necessary for the game-server scenario and does it serve productization"**. The
assessment is based on direct reading of the current code (not paraphrased documents), with evidence given
as file:line.

Overall conclusion: **the architectural skeleton is healthy and the direction is right** — CAF used only as
an internal actor runtime and fully hidden from business, the deterministic single-threaded semantics of
one-VM-per-service, route descriptors + single-target session binding for client RPC, and layered mockable
time. The main problems are not in the skeleton but in **unsafe/unfriendly defaults, a few declarations not
matching the implementation, and two capabilities genuinely missing for game scenarios (the DB async path
and hot updates)**. No refactoring for refactoring's sake; what is needed is to give "already-correct
designs" safe defaults, and to either implement or un-declare the "declared but not done" items.

Quick per-dimension verdicts:

| Dimension | Verdict | Main risk |
| --- | --- | --- |
| Process/thread model | sound | the net single-thread default is a throughput ceiling (low) |
| Network layer & protocol | sound | max_frame_size default unlimited (high), no TCP_NODELAY (medium) |
| Sessions & state management | sound and necessary | — |
| Data storage & persistence | sound layering, execution lacking | the DB ABI is fully synchronous and blocking (high) |
| Timers & task scheduling | sound | — |
| Hot updates & restart policy | lacking (annotated) | blue-green is design-only (medium) |
| Extension points | sound | manifest ceremony is heavy (low) |
| Fault tolerance & monitoring | good | process-level abort granularity is coarse (an accepted design) |
| Horizontal scaling | P0 (scope declared) | global is in-process only (declared, low) |

---

## 1. Process/Thread Model

**Current state**: single process, multi-threaded. The CAF scheduler thread pool (`caf.scheduler.max-threads`)
carries all service actors; a boost::asio `io_context` + dedicated net threads carry the client-facing
network (bootstrap.cpp:1246-1255: N threads when `net.threads > 0`, otherwise **single-threaded legacy
mode**); plus spawn worker threads and a total-budget watchdog (detached thread,
`std::_Exit(70)` on timeout). The client network and CAF's IO middleman are fully separated — the middleman
is for clusters only, which is right: the client-facing protocol (4 framings + codec plugins) should not be
hostage to CAF's serialization format.

**Verdict: sound**. A single-node-first game server needs no multi-process; the actor model + per-service
single-threaded execution semantics give business **lock-free determinism of state**. skynet is isomorphic
(worker threads + socket threads).

**Risks**:
- [Low] net defaults to single-threaded (when `net.threads` is unconfigured, bootstrap.cpp:1255 runs only
  one `net_io.run()`). With high connection counts, reads/writes share one io_context with console/http and
  starve each other. Suggest defaulting to 2 (or per core count) and documenting the tuning. Product
  impact: invisible at small-to-medium scale; config change needed before scale-up.

## 2. Network Layer and Protocol

**Current state**: asio TcpListener/Session; 4 framings (lenprefix/idlen/typed_len/delimiter), wire-header
`route_id` + pure business body; codecs go through C ABI plugins (`shield.protocol.codec.v1`); the Gateway
does 5 outbound validations + inbound route validation (existence/direction/requires_auth).

**Verdict: sound**. route_id in the wire header rather than embedded in the body lets forwarding decode
nothing — a must-have design for gateway-class services; the pluggable codec keeps heavy dependencies like
protobuf/flatbuffers out of the core, serving productization (the core distribution can ship without them).

**Risks**:
- [High] `max_frame_size` defaults to 0 = **unlimited** (include/shield/net/listener.hpp:88
  `// 0 = unlimited`; session.hpp:153 the same). A malicious/errant client sending one huge length prefix
  makes the server allocate buffers accordingly. For public listeners this is a clear DoS surface.
  **Recommendation: default to a 16MB cap (backstopped by the config validation layer), overridable by
  explicit config**. This is a "safe defaults" problem, not an architecture problem — low fix cost, direct
  product benefit.
- [Medium] No `TCP_NODELAY` anywhere in the codebase (zero hits in src/net/). Nagle is on; the small-packet
  round-trip latency is a perceptible degradation for turn-based/realtime games. asio sockets default to
  Nagle on. Suggest uniformly `set_option(tcp::no_delay(true))` after accept.
- ~~[Medium] No connection-level rate limiting / blocklist.~~ **Resolved (2026-09-26)**:
  `network.rate_limit` (per-connection token bucket, billed per decoded message, over-limit frames dropped
  without disconnect) and `network.blocklist.deny` (reject by address/CIDR at accept, entries validated at
  startup) have both landed; semantics in [Security Runtime Semantics](runtime-security.md).

## 3. Sessions and State Management

**Current state**: session established → initial target = the entry service declared by the listener; auth
goes through `shield.client.bind` performing the Gateway's **epoch CAS** atomic switch of the single target
(usage visible in auth.lua); thereafter all c2s RPCs go straight to that target service; s2c replies pass
5 outbound validations. one-player-one-service (one Lua VM per player).

**Verdict: sound and necessary for game scenarios**. The single target + epoch defends against the
"old/new session race at the auth-switch instant" — a correctness problem specific to game servers
(room switches / reconnects / kicks); the CAS semantics + outbound direction enforcement (replies on the
same route_id rejected by `egress_direction_rejected`, with a test anchor) turn "protocol direction written
wrong" into explicit failure. one-player-one-service buys lock-free player state — exactly skynet's core
model, retained as-is by Shield.

**Risks**: none major. The kick/reconnect semantics details (kick timing) should be made explicit in the
gateway doc later — a documentation item.

## 4. Data Storage and Persistence

**Current state**: persistence is explicitly a business-layer concern (runtime-persistence.md lists 6
storage patterns); the runtime provides only the DB plugin ABI (`shield.database.v1`:
connect/disconnect/ping/query/execute/begin/commit/rollback/free_result).

**Verdict: the layering judgment is sound; execution is lacking**.

- Sound layering: no ORM, no built-in storage (the repo has explicitly deleted the ORM direction); game
  genres differ greatly, so leaving storage to business + plugins is the correct tradeoff for
  "batteries-included" — the product can ship a distribution without MySQL.
- [High] **The ABI is fully synchronous and blocking, with no async/callback path** (the database.h table
  has no callback parameters). Consequence: a DB call initiated in any Lua service **wedges that service's
  entire VM** — a single-player service stalling itself is tolerable, but one slow query in a shared
  service (world/ranking) = whole-server stutter. skynet likewise has no built-in DB, but its conventions
  (suspending agents + a driver service) isolate blocking in a separate service; Shield's docs had not yet
  made "DB calls must live in a dedicated service / must have timeouts" a hard rule. **Recommend a
  two-step approach: (a) immediately document the usage discipline + example (wrap the DB in a dedicated
  service; business accesses via shield.call); (b) Phase 2 adds async entries to the ABI (callback or
  future + coroutine resume)**. This is not refactoring; it is closing a known gap. (a) is done
  ([DB Usage Discipline](db-discipline.md)); (b) is landed
  ([DB Async Entry Points](db-async-design.md), coroutine-resume style, M1–M4; the discipline document has
  been downgraded in step per the landing plan — the Lua side is no longer fully synchronous, and the
  C vtable keeps its synchronous primitives unchanged).

## 5. Timers and Task Scheduling

**Current state**: timers driven by CAF delayed send (ActorTimerState); business time is layered
(Clock/MockClock mockable business clock; the real monotonic clock only for scheduling); `shield.call` /
`shield.sleep` / timer callbacks / fork tasks / client RPC handlers are all coroutine-based — they may
sleep/call inside without blocking the actor; pending_calls carry timeouts and a requeue counter.

**Verdict: sound**. Coroutine-based blocking semantics are the hardest part of the skynet family to get
right, and Shield made "handlers may sleep" the default path rather than an advanced feature; the mockable
business clock makes "event start / day rollover" logic testable (skynet has no such layering).

**Risks**: no structural risk. The watchdog's hard `_Exit(70)` loses logs in extreme scenarios; forensics
compensates; acceptable.

## 6. Hot Updates and Restart Policy

**Current state**: no in-process hot-update command; the design goes blue-green service replacement (new
instance up → migrate → old instance exits, runtime-lua-vm.md); at the fault-tolerance layer per-service
on_error → consecutive error counting (threshold 10) → on_panic → `exit("panic")`; process-level
`lua_atpanic` = forensics dump + abort (lua_panic.hpp).

**Verdict: lacking**. skynet's `clear` hot update is its signature; Shield substitutes blue-green for
in-place patching, a model better suited to one-VM-per-service (state travels with the instance; no
code/state entanglement), **but today there is only the design draft**. For productization this is P2: a
new user's first 10 minutes don't need it, but the ops period certainly will. Until it lands, the docs
must keep the current non-promising stance (the current state does).

The fault-tolerance verdict is **good**: per-service panic isolation (a bad script only exits its own
service) + a process-level abort backstop + forensics suits a product better than skynet's default
(errors printf all the way out). The coarse abort granularity (a single VM's unrecoverable error = the
whole process dies) is a known tradeoff, acceptable with systemd/k8s restart policies; the docs should say
so.

## 7. Extension Points

**Current state**: Plugin System v1 — manifest-first, stable C ABI (codec / database etc.), explicit
instances and bindings, dependency injection; optional modules (cluster/global/player/server/ops) behind
build-time switches. The core is forbidden from depending in reverse on lua/net/plugin (the architecture
overview constraint).

**Verdict: sound**. A C ABI with dlopen is more productized than skynet's "C modules linked at compile
time" (a distribution can carry binary plugins; versions can align); build-time optional modules control
the core's size.

**Risks**:
- [Low] The manifest-first ceremony is heavy (two extra declarations for users who just want "a JSON
  codec"). The official plugins already embed enough examples; leave it for now; if feedback concentrates
  later, add a "built-in codec without manifest" shortcut.

## 8. Fault Tolerance and Monitoring

**Current state**: see section 6's fault-tolerance side + the monitoring plane on by default (HTTP `/ops/*`
7 routes + console command surface: status/services/plugins/config/cluster/server/global/log.level/attach
REPL/eval/lua.inspect/snapshot/diff); config errors fail fast at startup; `--check-config` validates
offline; the bootstrap teardown order was empirically fixed for three dangling classes under ASan
(archived in todo.md).

**Verdict: good**. skynet by default has only a debug console; Shield's default monitoring plane
(health/metrics/inspect/snapshot) covers ops usability, not just development debugging. The teardown order
has three backstops (monitor + down_msg + the 5s valve).

**Risks**:
- [Medium] **Declaration/implementation mismatch: sandbox**. config/app.yaml declares
  `lua.sandbox.allow_os/allow_io: false`, but VM startup **unconditionally** opens
  `shd::lib::io` and `shd::lib::os` (src/lua/lua_runtime.cpp:194-202);
  runtime-security.md also admits it is unimplemented. For a product this is an honesty problem: **a
  config key that is declared but has no effect** is worse than "no sandbox" (users think it's off when
  it's on). Suggestion: implement the switch (conditional open per config — low cost) or delete the keys.
  The former is preferred — teams whose scripts come from designers/outsources need it. **Landed this
  round**.
- [Low] metrics conventions undocumented (there is a /ops/metrics endpoint; the content semantics are
  unwritten).

## 9. Horizontal Scaling

**Current state**: the cluster optional module (static peers + CAF middleman, M1-M5 landed);
the global module is currently an **in-process P0 backend** (in-memory KV/locks/leaderboards/queues/cron),
with the Redis backend declared for Phase 2.

**Verdict: the P0 scope is clear**. Single-node-first is the stated product positioning; global giving
single-process semantics first lets the API stabilize before swapping in a distributed backend — the right
order. The risk is only expectation management: the docs already say it clearly (global_manager.hpp
comments + roadmap); acceptable.

---

## Which Designs Are Necessary / Excessive / Lacking (game-server perspective)

**Necessary and done right** (keep; don't touch):
- per-service single thread + one-VM-per-player (lock-free state — skynet's core value)
- single-target session binding + epoch CAS + outbound direction enforcement (game-specific correctness)
- coroutine-based call/sleep/timer/RPC handlers (natural business style + no actor blocking)
- route_id in the wire header + pluggable codecs (zero-decode forwarding + a light core)
- config fail-fast + --check-config (product-grade config experience)
- /ops/* and console on by default (ops out of the box)

**Over-engineering**: basically none. If pressed: the plugin manifest's declaration ceremony is heavy for
minimalist users (a low-priority observation item); the long-term maintenance cost of the typed_len and
delimiter framings needs user volume to justify (kept for now).

**Lacking** (by priority):
1. Safe defaults: max_frame_size default unlimited (fixed this round); TCP_NODELAY (fixed this round);
   connection rate limiting + address blocklist (fixed this round, see §2 resolved item)
2. the sandbox switch declared but unimplemented (fixed this round — product honesty)
3. the DB async path (landed: the discipline doc + [coroutine-resume async entry points](db-async-design.md) M1–M4)
4. hot-update blue-green landing (P2, separate project)
5. net.threads default and tuning documentation (brought out in docs this round)

## Improvement Roadmap (serving productization, not refactoring the skeleton)

| Phase | Content | Tradeoff rationale |
| --- | --- | --- |
| This round P0 | safe defaults (frame cap/NODELAY) + sandbox switch implementation + observable default config + template project + linearized onboarding path (details in product-gap.md) | all are small-change-big-benefit "defaults/honesty" problems; the architecture skeleton is untouched |
| Phase 1 | DB usage discipline doc and examples; the metrics conventions doc; net.threads tuning guidance; rate limiting design | mostly docs and low-risk changes; mark the known pits first |
| Phase 2 | DB ABI async entries (landed, M1–M4); connection-level rate limiting; blue-green hot update landing; TLS | each a separate project, all with existing design drafts or a doc place; scheduled by product feedback |

**Explicitly not doing** (this round): switching to a multi-process model, introducing ORM/event bus/middleware
(deleted directions), exposing CAF to business, refactoring for coverage. The architecture skeleton matches
the product positioning today; the assessment conclusion is "close the defaults and gaps", not "change the
structure".
