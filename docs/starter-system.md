# Starter System Design

This document defines the post-refactoring C++ startup orchestration system. Starters only initialize, start, and shut down the already-decided runtime modules in dependency order; they provide no DI, plugins, annotations, conditional wiring, or framework event bus.

## Positioning

The Starter is a bootstrap-internal mechanism, not a user API.

It is responsible for:

- Fixing the module startup order.
- Passing each module's parsed configuration to it.
- Failing fast and rolling back started modules on failure.
- Releasing resources in reverse order at shutdown.
- Centralizing Lua VM and Lua API binding initialization in `ScriptStarter`.

It is NOT responsible for:

- A DI/IoC container.
- Annotations or conditional wiring.
- Lua plugin loading.
- A lifecycle event bus.
- Service discovery, metrics, health, or management endpoints.
- The business lifecycle of Lua Services. Lua Services are managed per service semantics by `shield_core` and `shield_lua`.

## Core Objects

### BootstrapContext

`BootstrapContext` is the explicit context of the bootstrap phase. It is not a service locator and offers no global by-type lookup; each Starter reads only the dependency fields it declares.

```cpp
struct BootstrapContext {
  RuntimeOptions options;
  Config config;

  std::shared_ptr<LogRuntime> log;
  std::shared_ptr<ShieldCore> core;
  std::shared_ptr<DataRuntime> data;
  std::shared_ptr<NetRuntime> net;
  std::shared_ptr<TransportRegistry> transport;
  std::shared_ptr<LuaRuntime> lua;
};
```

Rules:

- Only bootstrap owns the `BootstrapContext`.
- Starters do not find dependencies through global singletons.
- A module that needs a dependency must receive it explicitly via constructor or `start()` parameters.
- `shield_core` cannot look Lua, net, data, config, or log back up from the `BootstrapContext`.

### IStarter

```cpp
class IStarter {
public:
  virtual std::string_view name() const = 0;
  virtual std::span<const std::string_view> dependencies() const = 0;

  virtual Result<void> configure(BootstrapContext& ctx) = 0;
  virtual Result<void> start(BootstrapContext& ctx) = 0;
  virtual void stop(BootstrapContext& ctx) noexcept = 0;

  virtual ~IStarter() = default;
};
```

Rules:

- The Starter list is statically registered by `shield_bootstrap`.
- Starters are not discovered from dynamic libraries, Lua scripts, or configuration.
- `configure()` only validates configuration and does light preparation; no threads are started, no ports bound.
- `start()` may allocate resources, start threads, and create runtime objects.
- `stop()` must be noexcept; on failure it can only log.

## Standard Starters

| Starter | Product | Dependencies | Notes |
| --- | --- | --- | --- |
| `ConfigStarter` | `Config` | none | parse CLI, load YAML, expand environment variables, validate the core schema |
| `LogStarter` | `LogRuntime` | `ConfigStarter` | initialize log sinks and the log level |
| `CoreStarter` | `ShieldCore` | `ConfigStarter`, `LogStarter` | create the service registry, message router, timer scheduler, CAF adapter |
| `DataStarter` | `DataRuntime` | `ConfigStarter`, `LogStarter` | create raw DB/Redis access capabilities per configuration; may be disabled |
| `TransportStarter` | `TransportRegistry` | `ConfigStarter`, `LogStarter` | register built-in transports and user-linked C++ transports |
| `NetStarter` | `NetRuntime` | `ConfigStarter`, `LogStarter`, `TransportStarter` | create listener/session management, but do not accept by default |
| `ScriptStarter` | `LuaRuntime` | `ConfigStarter`, `LogStarter`, `CoreStarter`, `DataStarter`, `NetStarter` | create the Lua VM strategy, register the `shield.*` API |
| `ServiceStarter` | configured services | `CoreStarter`, `ScriptStarter` | spawn the YAML-declared Lua/C++ services |
| `AcceptStarter` | active listeners | `NetStarter`, `ServiceStarter` | begin network accept after services are ready |

Optional modules such as `ClusterStarter`, `OpsStarter`, and `GlobalStarter` must be declared in separate official modules and must not enter `shield_core` or the default Starter list. `shield_server` needs no Starter: ServerManager is a process-level singleton, constructed directly by bootstrap and `mark_ready` at init complete (see runtime-server.md).

## Startup Order

```txt
parse command line
-> ConfigStarter.configure/start
-> LogStarter.configure/start
-> CoreStarter.configure/start
-> DataStarter.configure/start
-> TransportStarter.configure/start
-> NetStarter.configure/start
-> ScriptStarter.configure/start
-> ServiceStarter.configure/start
-> AcceptStarter.configure/start
-> enter event loop
```

Key constraints:

- Network listeners may be created early, but must wait for `ServiceStarter` to complete before accepting.
- Lua API bindings happen only in `ScriptStarter`.
- YAML-declared Lua services spawn only in `ServiceStarter`.
- On startup failure, stop in reverse order of successfully started Starters.

## ScriptStarter

`ScriptStarter` is the origin of the Lua VM lifecycle, binding C++ runtime capabilities into the `shield.*` API.

Responsibilities:

- Create `LuaRuntime`, a VM pool, or a VM factory.
- Load the standard Lua bootstrap chunk.
- Register the base API: `shield.log.*`, `shield.config`, `shield.now`.
- Register the core API: `shield.spawn`, `shield.exit`, `shield.self`, `shield.names`, `shield.query`, `shield.register`, `shield.unregister`, `shield.send`, `shield.call`, `shield.call_timeout`.
- Register the timer API: `shield.timer`, `shield.timer_once`, `shield.cancel_timer`, `shield.sleep`, `shield.fork`.
- Register the single-VM internal utility API: `shield.event`. It dispatches synchronously only within the current Lua state, never across service/VM/node, and carries no lifecycle.
- Register the gateway session API.
- Plugin APIs are not hard-coded by `ScriptStarter`; started plugins register their own `shield.<namespace>` via `register_lua`.
- Register an explicit `module_unavailable` error for disabled modules instead of a silent nil.

Non-responsibilities:

- Does not load plugin scripts.
- Does not publish lifecycle events.
- Does not create business services.
- Does not decide the service startup order.
- Does not fetch dependencies from a global ApplicationContext.

## Lua VM Strategy

Default strategy:

- One Lua state per service.
- The corresponding Lua state is released when the service exits.
- The read-only standard library and C++ binding descriptions are shared, but Lua global business state is not.
- Passing Lua object pointers across services directly is forbidden.
- Data between services must go through MessagePayload serialization.

Config example:

```yaml
lua:
  vm:
    mode: per_service
    max_vms: 10000
    max_memory_mb: 64
  sandbox:
    allow_os: false
    allow_io: false
```

## Error Handling

Starter errors must be structured:

```cpp
struct StarterError {
  std::string starter;
  ErrorCode code;
  std::string message;
  std::vector<std::string> context;
};
```

Failure policy:

- Config error: refuse to start.
- Core initialization failure: refuse to start.
- Data enabled and connection fails: refuse to start, unless the datasource is marked optional.
- Net port binding failure: refuse to start.
- Lua API binding failure: refuse to start.
- A single configured service fails to start: `actors[].required` decides continue or fail; default is required.

## Shutdown Order

```txt
stop accepting network connections
-> drain configured services through on_shutdown(ctx)
-> stop configured services through on_exit(reason)
-> stop Lua runtime
-> stop net runtime
-> stop transport registry
-> stop data runtime
-> stop core runtime
-> flush log
```

Rules:

- After network accept stops, existing sessions enter draining.
- The service drain phase calls `on_shutdown(ctx)`, bounded by `shutdown.timeout.service_drain`; this hook may run coroutine-aware `shield.call` within the deadline.
- After drain completes, fails, or times out, the service stop phase calls `on_exit(reason)`, bounded by `shutdown.timeout.service_stop`.
- `on_exit` must not run a suspending `shield.call`.
- `ScriptStarter.stop()` only releases VMs; it carries no business saves.
- No lifecycle events are published; `on_shutdown` is a service hook invoked directly by the runtime, not an event bus.

## Trade-offs Against the Old System

The following old designs are deleted outright, with no compatibility layer:

- `ApplicationContext` as the global component registry.
- The DI/IoC container.
- Annotations and conditional wiring.
- The old Lua plugin execution model and the old `shield.plugin.list/by_type/loaded/capabilities`.
- The C++/Lua lifecycle event bus.
- Discovering Starters dynamically via configuration.

Plugin System v1's `shield.plugin.*` is read-only introspection only; it is not a Starter dynamic discovery mechanism.

Only two ways to extend the runtime are allowed:

- Add an explicit Starter in an official module and include it explicitly in the bootstrap static list.
- Register transports, C++ services, or optional modules explicitly at the C++ program entry point.
