# Performance Design

This document records the performance design direction; no systematic benchmarking has been done yet, and concrete numbers are to be filled in.

## Core Principles

- Small core first; reduce startup path dependencies.
- Lua business logic does not touch CAF directly.
- Prefer `send` for business messages; avoid unnecessary synchronous waits.
- Every `call` must have an explicit timeout.
- The network layer only handles connections and byte streams; it does not carry business policy.
- Data access stays raw; avoid the extra complexity an ORM would introduce.

## Paths To Evaluate

- Cost of converting between Lua tables and C++ payloads.
- Blocking semantics of `shield.call` inside Lua coroutines.
- Upper bound on the number of scheduled timers.
- Cost of TCP / UDP / WebSocket session management.
- Number of copies in transport framing and encryption paths.
- Whether raw DB / Redis calls block runtime threads.

## Non-Core Performance Items

The following are not part of the current core performance goals:

- Prometheus metrics overhead.
- HTTP management endpoint latency.
- Middleware chain overhead.
- Service discovery backend latency.
- Plugin loading cost.

If these capabilities are added later as extensions, they should get their own performance budget.
