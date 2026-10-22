# Crash Reporting (Crashpad)

> Status: **design settled; implementation landing in batches**. Selection: Google **Crashpad** (Breakpad kept
> only as historical comparison, not introduced). Scope: process crashes (SIGSEGV / SIGABRT etc.) captured as a
> minidump **written locally** + offline symbolization to restore call stacks. Upload is a placeholder, off by
> default, never sent out.

## Problem

When the runtime crashes (null deref, assertion, memory corruption), a bare signal termination code cannot
be localized: core dumps depend on ulimit and deployment-side configuration and grow uncontrollably huge.
We need a capture plane that ships with the artifact, has an **independent process** write a compact
minidump at crash time, and restores call stacks offline from symbols afterwards.

## Selection

| Item | Decision |
| --- | --- |
| Framework | Google **Crashpad** (same as Chromium/Android), direct C++ integration |
| Comparison | Breakpad kept only as historical comparison; no code introduced |
| Integration | **git submodule** (the vcpkg manifest stays frozen pending a ruling, untouched; local vcpkg manifest install is a dead end — verified) |
| Symbol toolchain | rust `dump_syms` + `minidump-stackwalk` (installed on build machines/CI, not a repo dependency) |

Crashpad's key capabilities match the needs exactly: **out-of-process handler** (the capture process is
isolated from the crashing process and unlikely to co-crash), minidump format compatible with Breakpad
(dump_syms / minidump-stackwalk work directly), and a packaging shape of a client library + a standalone
handler executable.

All three submodule gitlinks are registered in the shield main repo with a **flat layout** (CI fetches them
with one `submodules: recursive`):

| Path | Source | pin |
| --- | --- | --- |
| `third_party/crashpad` | github.com/chromium/crashpad | `677b9e33` (main tip) |
| `third_party/mini_chromium` | github.com/chromium/mini_chromium | `c0299833` (== crashpad DEPS pin) |
| `third_party/lss/lss` | chromium.googlesource.com/linux-syscall-support | `9719c1e1` (== crashpad DEPS pin, tag v2022.10.12) |

Why not crashpad's native nested DEPS layout: gitlinks are leaves in the git index and `git submodule add`
refuses paths inside a submodule (verified by test), so nested gitlinks cannot be expressed. crashpad's
include shapes are all compatible with the flat layout — `"base/…"` (short form, resolved by
`-I third_party/mini_chromium`), `"third_party/lss/lss.h"` (wrapper header inside the crashpad tree,
resolved by `-I third_party/crashpad`), and the wrapper-expanded
`"third_party/lss/lss/linux_syscall_support.h"` (resolved by `-I <repo root>` to
`third_party/lss/lss/`). All include resolution is taken over by CMake glue (below); crashpad's own build
is not used.

## Architecture

```
shield 进程                             crashpad_handler 进程（独立）
┌─────────────────────────┐            ┌──────────────────────────┐
│ crash::initialize()     │  spawn+pipe│ 等待崩溃事件             │
│  └ Crashpad Client ─────┼────────────┤                          │
│ 崩溃 → 信号/检查点 ─────┼────────────┤ 采内存快照写             │
│                         │            │  <dump_dir>/<uuid>.dmp   │
│ 启动扫描 <dump_dir> ────┼── 记 WARNING 日志（遗留 dump=上轮崩溃）│
└─────────────────────────┘            └──────────────────────────┘
        离线：dump_syms(二进制) → <name>.sym → minidump-stackwalk(dump)
```

## Handler Initialization Point

The initialization order is fixed inside `bootstrap::initialize_impl()` (src/bootstrap/bootstrap.cpp):

1. logger initialization and config file loading complete (after this the `crash.*` keys are readable);
2. `crash::initialize()` is inserted **before config validation and the subsystem starters** —
   this is the earliest point where "config available + logging available" intersect, and the whole of
   bootstrap runs under its protection;
3. Degradation policy: handler executable missing / dump directory cannot be created → log an **ERROR**
   and **continue starting** (capture is an enhancement; it never blocks the service).

`--check-config` semantics are "Initialize, validate, then shut down", so it also passes through
initialization — incidentally self-checking the handler executable and dump directory writability — and
then shutdown reclaims the handler normally.

The build switch `SHIELD_ENABLE_CRASHPAD` (**default ON**, artifacts carry the handler):

- Compile side: the glue code in `src/crash/` and `include/shield/crash/` and all
  `#ifdef SHIELD_ENABLE_CRASHPAD` branches are trimmed by the switch;
- **Both coverage-gate jobs explicitly pass `-DSHIELD_ENABLE_CRASHPAD=OFF`** (following the existing
  convention of the four switches OFF): the gcovr filter surface and the three metrics (98 line / 100 branch
  / 100 function, optional line 100) are byte-for-byte identical to before this batch.

## Dump Directory

| Item | Value |
| --- | --- |
| Config key | `crash.dump_dir`, default `""` → `crash/` relative to cwd |
| In containers | WORKDIR `/app` → `/app/crash` (mount the directory on a volume to ship captures out) |
| File name | `<uuid>.dmp` (named automatically by the crashpad database) |
| Creation | created at initialization (crashpad database Initialize); failure = degraded ERROR |
| Repo | `/crash/` goes into .gitignore (dumps from local acceptance runs don't dirty git status) |

Startup scan: `crash::initialize()` enumerates existing `*.dmp` in the directory; when the count > 0 it logs
`SHIELD_LOG_WARNING` (logger name `crash`) listing each file — **leftover dumps from the previous crash are
the direct signal for an ops alert** (see "Integration with logging/monitoring").

## Standalone Handler Process Packaging

- CMake targets: static library `crashpad_client` + executable `crashpad_handler`
  (glue in `cmake/crashpad.cmake`; source sets exclude win/mac/ios/tvos/fuchsia and `*_test.cc` per
  Linux/POSIX; include roots = crashpad, mini_chromium, repo root (the lss path) + crashpad upstream compat
  layers (`compat/linux`'s signal.h include_next shim adds glibc's missing `SS_AUTODISARM`/`SA_EXPOSE_TAGBITS`;
  `compat/non_win` provides windows.h-shaped minidump headers), define
  `CRASHPAD_LSS_SOURCE_EMBEDDED`; non-Linux platforms are automatically OFF (the mac/win build matrix has
  zero regression, see "Non-goals").
- Artifact layout: `build/bin/shield` and `build/bin/crashpad_handler` in the same directory.
- Lookup order: explicit `crash.handler_path` config → default to the directory of the shield executable
  itself (derived from `/proc/self/exe`) plus `crashpad_handler` → if neither, degraded ERROR (see
  initialization point).
- Docker: builder-stage artifacts compile via `COPY . .`; the runtime stage does
  `COPY --from=builder /build/build/bin/crashpad_handler /app/`, same directory as
  `ENTRYPOINT /app/shield`; the dump directory defaults to `/app/crash`.
- CI: **every C++-compiling job** adds `submodules: recursive` to checkout (the docs job, pure vitepress,
  does not need it).

## Symbol Table Management

Principle: **symbols are separated from artifacts** — generated and archived at build time; the running
site carries no debug symbols.

| Step | Practice |
| --- | --- |
| Keep -g at build | `RelWithDebInfo` recommended (-O2 -g); acceptance/CI jobs build with it |
| Generate symbols | `dump_syms build/bin/shield > shield.sym` (DWARF → file+line level) |
| Restore stacks | `minidump-stackwalk crash/<id>.dmp --symbols-path <sym dir>` (the sym must be laid out in breakpad layout `<module>/<debug-id>/<module>.sym`; the script does this) |
| One command | `tools/symbolize-crash.sh build/bin/shield crash/<id>.dmp [sym dir]` (dump_syms + layout + stackwalk in one step) |
| Release image | the current Dockerfile Release has no DWARF, but **has a symtab → function-level call stacks are still restorable**; the image stays symbol-free (separation principle); for file+line use the `.sym` archived from the build machine's RelWithDebInfo |
| Archiving | `.sym` archived named by the binary's commit hash (`SHIELD_GIT_COMMIT_HASH` is injected into the version string; the dump annotation carries the same version, see below) |

The minidump-to-binary correspondence is guaranteed by two things: the module GUID/addresses embedded in
the dump + the version string written into annotations at startup.

## Upload Placeholder (off by default)

| Config key | Default | Semantics |
| --- | --- | --- |
| `crash.upload_url` | `""` | **Outbound traffic is data exfiltration; off by default**. This batch does not implement upload; a non-empty value only logs a WARN at startup ("configured but not implemented; dumps stay local") as a placeholder, so nobody assumes reporting is happening |

The placeholder semantics are fixed: when upload is implemented later it may only be enabled by explicit
configuration; the default never touches the network.

## Deliberate Crash Acceptance (null-deref switch)

- CLI switch `shield --crash-test` (no value): after full initialization succeeds, first log one ERROR line
  (pre-crash logs hit disk), then deliberately dereference a null pointer
  (`crash_test_null_deref()`, a standalone function so symbolized stacks identify it easily).
- Builds with crashpad OFF (the coverage jobs are exactly that): the flag resolves to a clear
  "requires a crashpad build" error with test coverage (new branches all on the OFF side).
- Acceptance assertion chain:
  1. the process exits abnormally (signal termination);
  2. `*.dmp` appears in `<dump_dir>`;
  3. the call stack restored by `dump_syms` + `minidump-stackwalk` **contains the
     `crash_test_null_deref` frame and the `shield::run` frame** (symbolization works);
  4. the startup log shows the WARNING scan line for the previous round's leftover dump (looping back to ④).
- A new crashpad CI job: RelWithDebInfo build → run `--crash-test` → assert the four items above →
  minidump + stack output archived as artifacts.

## Integration with Existing Logging/Monitoring

No new monitoring endpoints; all integration goes through the existing loggers (logger names `crash`,
`bootstrap`):

| Timing | Level | Content / alerting use |
| --- | --- | --- |
| Initialization success | INFO | dump directory, handler path (capture plane ready state) |
| Initialization degraded | ERROR | handler missing / directory uncreatable → **capture has failed**; alert keyword `crash collector degraded` |
| Startup scan finds leftover dumps | WARNING | `pending crash dump` + file list → **previous crash**; alert keyword |
| upload_url set non-empty | WARN | upload unimplemented placeholder |
| Pre-crash (--crash-test and real crashes share the path) | ERROR | the last pre-crash log line aligns with the dump time |

Ops plane: the dump directory can be volume-mounted/packaged for retrieval directly; the log keywords are
the alert rules (pending crash dump / collector degraded), isomorphic to existing log-based monitoring.

## Configuration Reference

```yaml
crash:
  enabled: true           # default false; enable explicitly in app.yaml (artifacts carry capture by default)
  dump_dir: ""            # empty = ./crash (relative to cwd)
  handler_path: ""        # empty = crashpad_handler next to shield
  upload_url: ""          # placeholder: non-empty only logs a warning, nothing is sent (data exfiltration off by default)
```

Test fixture configs omit the `crash:` section → everything defaults to false: test binaries spawn zero
handlers, and the existing tests that deliberately trigger fatal signals (si_addr assertion types) are
unaffected.

## Non-goals

- No upload implementation (placeholder as above), no symbol server (archiving `.sym` files suffices);
- No Windows/macOS acceptance (the glue excludes per POSIX; Linux goes first);
- No hang/timeout capture (a separate monitoring track; neither crosses the other's boundary).
