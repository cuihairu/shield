# Documentation Map

This document explains the reading order, authority precedence, and current-state conventions of the Shield documentation. When documents disagree, judge by the precedence on this page.

## Document Status

| Status | Meaning |
| --- | --- |
| Authoritative contract | Stable boundaries that both users and implementations must follow. Code, tests, and examples should converge toward these documents. |
| Runtime semantics | Refined rules for a specific runtime subsystem. If it conflicts with the overview, the overview wins; if it conflicts with an example, the semantics document wins. |
| Reference | Operational explanations such as plugins, deployment, development, and ops. It should reflect the current implementation and does not define cross-module architecture principles. |
| Draft/archive | Design directions, business patterns, or future toolchains. Must not be treated as currently implemented runtime capability. |

## Authority Precedence

1. Architecture Overview: module boundaries, dependency direction, core non-goals, and extension principles.
2. Lua API Contract, Runtime Config Semantics, Error Code Reference: user-visible APIs, configuration, and errors.
3. Plugin System v1, Plugin Reference: plugin packages, instances, bindings, ABI, and official plugins.
4. Each `runtime-*.md`: semantics of subsystems such as services, messages, timers, the Lua VM, network, data, logging, bootstrap, and security.
5. Roadmap and [Decision Log](open-decisions.md): current phase, closed decisions, and upcoming scope.

## Recommended Reading Paths

New users read first:

- [Quick Start](quickstart.md)
- Lua API Contract
- Runtime Config Semantics
- Plugin Reference

Framework developers read first:

- Architecture Overview
- [Core Design Principles](architecture-core-concepts.md)
- [Runtime Semantics Decisions](runtime-semantics.md)
- [Development Guide](development-guide.md)

Plugin authors read first:

- Plugin System v1
- Plugin Reference
- Protocol Codec Plugins (only protocol codec provider authors need this)
- the official plugin reference page for the target interface header

Ops and deployment read first:

- [Deployment](deployment.md)
- Runtime Config Semantics
- [Runtime Log Semantics](runtime-log.md)
- Ops Semantics
- Lua Diagnostics Console Design

## Current Conventions

Shield's current stance is single-node-first, Lua-first, plugin-based backend capabilities. The core runtime and Plugin System v1 are on the current implementation path; official optional modules such as `shield_cluster`, `shield_global`, `shield_player`, `shield_server`, and `shield_ops` proceed per the roadmap.

The old DI/IoC, annotations, conditions, events, middleware chain, old plugin v0, and the old discovery/metrics/health core integrations are no longer compatibility targets. If related historical documents still carry the old stance, this page, the Architecture Overview, and the Roadmap prevail.

## Document Sections

| Section | Purpose |
| --- | --- |
| Getting started & status | Quick start, roadmap, FAQ, deployment, and development entries. |
| Authoritative contracts | Overview, Lua API, configuration, error codes, plugin system, and test matrix. |
| Core runtime | Services, messages, timers, Lua VM, network, data, logging, bootstrap, security, and gateway. |
| Plugin reference | Official plugins and third-party plugin development guidance. |
| Official optional modules | Extension semantics outside core such as cluster/global/player/server/ops, including the cluster implementation plan (with landed implementation records). |
| Engineering reference | Directory structure, CMake, CAF mapping, performance, best practices, and tutorials, including the engine SDK design (reviewed and approved). |
| Drafts & archive | Future directions or business-pattern references such as the schema toolchain, xmldef toolchain, xmldef descriptor/compiler/runtime specs, Unity generator spec, base-component/runtime adaptation boundaries, entity schemas, persistence and rollback. |

The suggested reading order for the current `xmldef` special-topic drafts:

1. Xmldef Toolchain Design
2. [Xmldef Descriptor Spec](xmldef-descriptor-spec.md)
3. Xmldef Phase 1 Implementation Plan
4. [Xmldef Compiler / Runtime MVP](xmldef-compiler-runtime-mvp.md)
5. [Xmldef Unity Generator Spec](xmldef-unity-generator-spec.md)

For protocol layering terminology, the `ProtocolProfile` model, and the current support matrix, Protocol Routing Design is the special-topic entry. For the protocol codec plugin ABI, the default built-in boundary, and the protobuf-first landing scope, see Protocol Codec Plugins.

For DB usage discipline see [DB Usage Discipline](db-discipline.md) (after the async entry points landed, rules 1/2 are downgraded to recommendations; 3–6 remain hard); for the coroutine-resume async entry design and milestone status, [DB Async Entry Points](db-async-design.md) is the special-topic entry (implemented, M1–M4).

For client-facing TLS (the `network.tls` configuration surface, handshake/timeout semantics, certificate loading and hot-reload tradeoffs), [TLS Design](tls-design.md) is the special-topic entry (v1 implemented); for runtime usage see the TLS section of [Network Runtime Semantics](runtime-network.md).

For crash capture (Crashpad selection, handler initialization point, dump writing, symbol separation, upload placeholder off by default), [Crash Reporting](crash-reporting.md) is the special-topic entry.

For later discussions of Lua primitives, cosocket-style outbound I/O, CAF adapter reuse boundaries, and the negative conclusion on `auth-first`, see the later draft [Base Components and Runtime Adaptation Boundaries](runtime-primitives.md).
