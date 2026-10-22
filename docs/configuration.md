# Configuration Design

This document only explains the configuration layering and does not re-define the schema. The authoritative source for Shield's current **core runtime configuration** is Runtime Config Semantics.

The current source tree and historical configs may still contain legacy fields; those fields must not be treated as the current refactoring contract.

## Configuration Layers

### Core Schema

core only defines the configuration required by a single-node runtime:

- `app`
- `log`
- `lua`
- `actors` (including the gateway actor's `network` listener/profile and `rpc.routes` descriptor declarations)
- `plugins`
- `bootstrap`
- `shutdown`

Field-level rules, validation, hot-reload boundaries, and merge strategy are uniformly governed by runtime-config.md.

### Optional Module Schema

The following configuration sections are not part of the core schema and are interpreted separately by the corresponding official optional modules:

- `cluster` -> Cluster Semantics
- `global` -> Global Capabilities
- `ops` -> Operations & Debugging
- `player` -> Player Lifecycle
- `server_manager` -> [Server State](runtime-server.md)

The core bootstrap only passes these configuration snapshots to the enabled modules; it does no field interpretation or validation in core.

### Plugins Schema

The `plugins` section (`plugins.directory`, `plugins.instances`, `plugins.bindings`) belongs neither to the core schema nor to any optional module. It is read by `shield_bootstrap` and handed wholesale to the `PluginHost` of Plugin System v1: scanning, catalog, dependency resolution, config schema validation, and binding resolution all happen inside the plugin system. The core configuration layer only passes it through and does not interpret the fields.

## Minimal Example

```yaml
app:
  name: hello_world

log:
  level: info
  console: true

actors:
  - name: bootstrap
    script: scripts/bootstrap.lua
    instances: 1
    required: true
```

The default minimal configuration declares no `plugins.instances`, so it starts without any provider shared library. When a data backend is needed, add `plugins.directory`, `plugins.instances`, and `plugins.bindings` per Plugin System v1.

## Removed Legacy Directions

The following fields and directions no longer enter the current core configuration design:

- `discovery`
- `metrics`
- `health`
- the old top-level `database` / `redis`
- `middleware`
- `conditions`
- `annotations`
- DI/IoC wiring configuration
- `network.protocol.routes` inline routing (replaced by the `actors[].rpc.routes` descriptors)

If genuinely needed in the future, they can only be re-defined in non-core extension documentation; they must not be stuffed back into `runtime-config.md` in reverse.
