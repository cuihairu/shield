# Product Gap Review

Assessment baseline: **a new user gets a minimal game server running within 10 minutes** (clone → build →
start → one request through with a client). Comparisons: skynet (primary reference), plus the out-of-box
experience of comparable batteries-included offerings (Nakama's `nakama new` + one-command docker,
Colyseus's `npm init` template path).

Conclusion first: **the engineering foundation (config validation, the ops plane, example quality) exceeds
expectations, but the "first successful run" path is blocked by three things — the first-build wall, the
default config having no observable behavior, and no template project generation**. These three were
exactly this round's top-3 to close (see "Closed Items" at the end).

| Dimension | Current rating | One-line conclusion |
| --- | --- | --- |
| One command to start the service | partial | `./build.sh run` exists, but the first build must climb the vcpkg source-compile wall |
| Default config runs with zero changes | partial | It runs but "runs to nothing": no game port, no observable behavior |
| Examples/template projects | partial | hello_world has real TCP e2e tests; lacks "generate my own project" |
| Install & deploy scripts | mostly met | build.sh/build.bat/Dockerfile all present; missing environment preflight |
| Logging & monitoring usable by default | met | console + /ops/* HTTP on by default (skynet by default has only a debug console) |
| Hot updates & ops commands | partial | ops commands cover REPL/snapshot/diff; hot updates are design-only (scope table at the end) |
| Documentation & onboarding path | partial | Contract docs are complete; but no 10-minute linear path exists, and the tutorial admits it is unverified |
| Error message friendliness | partial | Config errors fail fast with per-key messages; environment errors have no preflight |

---

## 1. Can one command start the service?

**Current state**: `./build.sh run` (build.sh) does build+start with the default config in one step; the
Dockerfile is multi-stage and runs the `--check-config` self-check during build. The command itself is
adequate.

**Gap**: the **first build** before the command is the real wall —

- vcpkg manifest mode compiles boost/CAF/openssl/lua/yaml-cpp etc. from source; even an 8-core machine
  takes tens of minutes; on weak networks vcpkg's own git clone and tool downloads (cmake tarball) can
  fail repeatedly (measured locally: early EOF / SSL 35, needing resume support as a fallback).
- No environment preflight for requirements: cmake ≥ 3.30, a C++23 compiler (gcc ≥ 13 / clang ≥ 17 / MSVC
  19.38+), ninja, VCPKG_ROOT. With any of them missing, the user sees raw cmake errors, and vcpkg failures
  cascade into **misleading secondary errors** like "CMake was unable to find Ninja" (measured locally,
  with ninja plainly present in /usr/bin).
- The Docker path is equally heavy (a full round of vcpkg compilation inside the build), and the
  `EXPOSE 8080 8081 8082 8083` comment does not match the default config's actual behavior (next section).

**skynet comparison**: `git clone && make && ./skynet examples/config.lua` — one make, a minute or two, no
package manager, no network dependencies (deps in-tree).

**Rating: blocking (within the top-3, under the "startup scripts and docs" item)**. The dependency tree
cannot be cut down to skynet's size (CAF/asio are settled architecture decisions), so the remediation
direction is: a build.sh environment preflight with actionable errors, plus documentation of the
prerequisite matrix and the vcpkg binary cache (`VCPKG_DEFAULT_BINARY_CACHE`) damage-control path.

## 2. Does the default config run with zero changes?

**Current state**: `config/app.yaml` starts with zero changes: a bootstrap actor (scripts/bootstrap.lua
prints one "started" log line) + console (/tmp/shield-console.sock) + HTTP ops (127.0.0.1:8080).
`--check-config` passes offline, and CI has two tests anchoring it (CMakeLists.txt:666,696).

**Gap**:

- **No observable game behavior**. After starting, the new user has no TCP port to connect to; the first
  "see it work" moment requires switching to examples/hello_world (another cmake configuration + another
  config). The default path and the example path are disconnected.
- The Dockerfile's trailing comment "Default ports: TCP 8080, UDP 8081, HTTP 8082, WS 8083" is a stale
  blueprint convention: the default config has no 8080 TCP game listener (8080 is http ops, and bound to
  127.0.0.1 — binding 127.0.0.1 in a container means unreachable from outside). Following the comment with
  `docker run -p` simply cannot connect — directly misleading.

**Rating: blocking (the first of the top-3)**. Remediation direction: add a minimal echo listener to the
default config (idlen framing + one c2s/s2c route) so that after `./build.sh run` an `nc`/example client
has something to connect to immediately; fix the Dockerfile comment and exposure conventions in step.

## 3. Are there directly runnable example/template projects?

**Current state (examples)**: `examples/hello_world/` is a complete loop — auth (TCP listener + pre-login)
→ player (single target after bind) → room (private forwarding + s2c replies), with real TCP e2e
acceptance tests (tests/hello_world_acceptance.cpp,
tests/acceptance/test_client_rpc_e2e.cpp). Example quality is above the skynet examples average
(most skynet examples carry no tests).

**Current state (templates)**: **none**. Starting one's own project requires: hand-copying hello_world's
main.cpp + writing one's own CMake to link shield + copying config/scripts — every step sheds users.
In the skynet ecosystem, "copy an example config and tweak it" is the mental default; Nakama has
`nakama new`, Colyseus has `npm init colyseus` — comparable products all treat scaffolding as standard.

**Rating: blocking (the second of the top-3)**. Remediation direction: `scripts/new_project.sh <dir>` in
one command generates a minimal project that passes `--check-config`, builds, and starts (the template
derives from the verified hello_world), with a ctest anchoring "the generated project's config must pass
validation".

## 4. Install and deploy scripts

**Current state**: build.sh (debug/release/run/clean modes), build.bat, a multi-stage Dockerfile (build-time
`--check-config` self-check, non-root at run time — both good practices).

**Gap**: the Linux bare-metal prerequisite list exists only inside the Dockerfile's apt commands; new users
have to dig it out themselves; no docker-compose / no preflight script.

**Rating: minor (merged into the top-3's scripts-and-docs item)**.

## 5. Logging and monitoring usable out of the box

**Current state**: the default config enables console logging; HTTP ops is on by default
with a full route set: `/ops/health` `/ops/status` `/ops/metrics` `/ops/services[/:name]`
`/ops/plugins` `/ops/config`, plus explicit opt-in `/ops/eval` `/ops/profile`
(src/console/ops_http_handler.cpp:163-221); the Unix console is on by default, with commands covering
`help` / `root.status` / `root.services` / `root.log.level` (tunable at runtime) /
`attach` (interactive REPL) / `eval` / `lua.inspect` / `lua.snapshot` / `lua.diff`
(src/console/root_commands.cpp, lua_commands.cpp). skynet by default has only a debug console (a node
listener).

**Watch-outs**: default-binding 127.0.0.1 / Unix socket is the correct secure default, but container and
remote deployment docs must say how to change it; the metrics content conventions doc is still missing.

**Rating: met**. This round only adds "document the existing capabilities clearly"; no new code.

## 6. Hot updates and ops commands

**Current state**: ops commands are rich (previous section; the attach REPL + snapshot/diff are things
skynet lacks). Hot updates exist only as a design draft (blue-green service replacement,
docs/runtime-lua-vm.md); there is no `reload`-style command.

**skynet comparison**: the `clear` mechanism + data-driven code loading are skynet's signature capability;
Shield's one-VM-per-service model in principle needs no in-place patching (swap the service instance), but
until blue-green lands, this gap is real.

**Rating: lacking** (it has a place on the roadmap; until blue-green lands the gap is real). **Not in the
top-3** — a new user's first 10 minutes do not need hot updates; no code for it now.

## 7. Documentation and onboarding path

**Current state**: documentation is extensive and contract-first (architecture/Lua API/config/error
codes/plugin ABI all have authoritative documents); quickstart.md honestly distinguishes "target
experience" from "current state".

**Gap**:

- **No 10-minute linear path**. quickstart is written as a "status report", not a "follow me"; verification
  commands are scattered across the "current state / acceptance criteria" sections, and readers must
  assemble the examples build command themselves.
- `docs/tutorial-game-backend.md` opens by admitting it is "not a currently verified runnable guide", and
  its YAML semantics diverge from hello_world's verified configuration (binding/route style not aligned).
  For newcomers this is a trap: copying the tutorial yields something that won't run.
- No client sample. hello_world's e2e is written in C++; the new user has nothing ready to "connect and
  try" (a 20-line python idlen client would suffice).

**Rating: blocking (the third of the top-3)**. Remediation direction: rewrite quickstart as a linear
10-minute path (prerequisites → build → start → connection check, every command actually tested); fix the
tutorial to point at the verified path or make it runnable; add scripts/client_demo.py.

## 8. Are error messages newcomer-friendly?

**Current state**: the config side is systematic — fail-fast at startup, per-key errors, offline
`--check-config`, CI-anchored; Lua script load errors take the protected path with a traceback (load_script
fixed; no longer aborts directly under NDEBUG).

**Gap**: the environment side has no preflight — insufficient cmake version, a compiler without C++23,
VCPKG_ROOT unset, network download failure — the user sees raw toolchain errors that cascade into each
other (see section 1's Ninja false alarm).

**Rating: partial**. Remediation direction: a build.sh preflight (version probing + a one-line "install
what" hint per missing item), merged into the top-3's third item.

---

## Priority Ranking (against the "running in 10 minutes" goal)

| # | Item | Rationale |
| --- | --- | --- |
| P0-1 | Observable default config (echo listener) | Without it, "running" cannot even be defined |
| P0-2 | One-command template project generation | The only bridge from "looking at examples" to "writing my own" |
| P0-3 | Startup script & docs linearization (preflight + quickstart rewrite + tutorial fix + client sample + Dockerfile conventions fix) | Strings the first two steps into one actually-tested path |
| P1 | Deployment docs (container port conventions, binary cache), metrics conventions doc | Damage control; does not block the 10 minutes |
| P2 | Landing hot-update blue-green | The skynet-parity item; a separate project (see architecture-review.md roadmap) |

## Closed Items (landed this round)

- [x] The default config gains an echo loop: `config/app.yaml` adds an `echo` actor
      (scripts/echo.lua, TCP 0.0.0.0:8001, idlen + json, c2s `echo` id=1 /
      s2c `echo_result` id=100, `requires_auth: false`) — connectable right after
      `./build.sh run`. CI's existing two `--check-config` anchors cover the default config, and the new
      `shield_default_config_boot_smoke` actually boots the default config and verifies the listener accepts.
- [x] Template project: `scripts/new_project.sh <dir>` generates a minimal project from the verified
      template (config + scripts + main.cpp + CMakeLists), automatically running
      `shield --check-config` after generation; `tests/new_project_scaffold.cpp` anchors
      "the generated project must pass config validation + have all files".
- [x] build.sh environment preflight: cmake ≥ 3.30 / C++23 compiler / ninja / VCPKG_ROOT probed
      item by item, with a one-line "install what" hint per missing item — vcpkg secondary errors no
      longer take the blame.
- [x] quickstart.md rewritten as the linear 10-minute path (every command actually tested);
      tutorial-game-backend.md fixed to point at the verified path; the Dockerfile's stale
      EXPOSE comment aligned with the actual conventions.
- [x] `scripts/client_demo.py`: a 20-line idlen+json client covering the echo and
      hello_world login→move loops, the default tool for the "connection check" step.
