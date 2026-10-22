# Development Guide

This document is aimed at developers working in the refactoring phase. The current goal is to unify the documentation, boundaries, and API contracts first, then adjust the source code step by step.

## Development Principles

- Read `docs/architecture.md` first.
- The Lua user API is defined by `docs/lua-api.md`.
- Lua API tests are designed per `docs/lua-api-tests.md`.
- CMake target splitting proceeds per `docs/cmake-refactor.md`.
- Do not keep treating the old modules as core facts.
- New code should converge around `actor`, `net`, `transport`, `script`, `timer`, `data`, `config`, and `log`.
- Do not add DI/IoC, plugins, service discovery, Prometheus, health check, or middleware chain dependencies to `shield_core` or the minimal main path.
- Multi-process / multi-machine capability belongs to the official `shield_cluster` optional module and must not pollute core in reverse.
- The Lua API must have binding tests.
- The user-facing API does not expose CAF.

## Current Source State

The source tree still keeps old architecture directories, for example:

- `discovery/`
- `metrics/`
- `health/`
- `di/`
- `annotations/`
- `conditions/`
- `events/`
- `plugin`-related code
- the standalone `protocol/`
- parts of the `gateway/` middleware code

These directories need to be deleted, merged, or moved to a non-core experimental area later.

## Target Directory Layout

```text
include/shield/
├── shield.hpp
├── actor/
├── net/
├── transport/
├── script/
├── timer/
├── data/
├── config/
└── log/
```

Official optional modules live in separate paths, for example `src/optional/shield_cluster/`, `src/optional/shield_global/`, and `src/optional/shield_ops/`.

## Building

The build flow still follows the current CMake setup, but the CMake targets also need to be adjusted along with the refactoring.

```bash
cmake -B build -S .
cmake --build build
```

Until `shield::run(argc, argv)` and the final CLI are frozen, do not write the current command-line shape down as stable user documentation.

## Pre-Commit Checklist

- Does the documentation still claim the old Phase 1-7 is complete?
- Is any non-core capability described as a default capability?
- Did core gain any new dependency on the old modules?
- Are cluster/global/ops mistakenly written as minimal main path dependencies?
- Do the examples conflict with the contracts in `docs/lua-api.md`?
