# protocol.msgpack

> MessagePack binary codec with the same optional per-route payload schema validation as protocol.json; about half the size of JSON.

`protocol.msgpack` is one of the official `shield.protocol.codec.v1` providers, with codec name `msgpack`. The core no longer bundles a msgpack codec — you **must** bind this plugin through `body.provider` to enable it. decode uses `from_msgpack` and encode uses `to_msgpack` (nlohmann/json's built-in MessagePack implementation; no external msgpack-cxx runtime dependency), with optional payload shape validation per route at the ingress/egress boundary.

**Selection at a glance**: semantically isomorphic to protocol.json but with a binary wire format — suitable when packet size/bandwidth matters and you don't want a schema compiler; for cross-language strongly-typed schema evolution pick protocol.protobuf; for flatc toolchain integration pick protocol.flatbuffers.

## Package Information

- **Package ID**: `protocol.msgpack`
- **Interface**: `shield.protocol.codec.v1` (capabilities: `msgpack`)
- **Version**: 1.1.0 (schema validation capability since v1.1.0)
- **CMake option**: `SHIELD_BUILD_PLUGIN_MSGPACK`
- **Source**: `plugins/protocol.msgpack/`
- **Dependencies**: [nlohmann/json](https://github.com/nlohmann/json) (header-only; MessagePack codec is a built-in capability, the plugin does not link msgpack-cxx. The vcpkg manifest feature `protocol-msgpack` still passes through and installs the `msgpack` port — a build-time behavior, not a runtime dependency)

## Build Enable

Turn on `SHIELD_BUILD_PLUGIN_MSGPACK` at the CMake configure stage:

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_MSGPACK=ON
cmake --build build
```

This option triggers two things:

1. The shared library under `plugins/protocol.msgpack/` is built, output to `plugins/protocol.msgpack/bin/`.
2. CMake passes the vcpkg manifest feature `protocol-msgpack` through to vcpkg.

CI's ci.yml and plugins-ci workflows both build with `SHIELD_BUILD_PLUGIN_MSGPACK=ON` (`test_protocol_msgpack_plugin`).

## Config Schema

The instance config has four keys, all optional, fully isomorphic to protocol.json:

| Field | Type | Required | Default | Description |
|------|------|------|--------|------|
| `schemas` | object | no | — | Inline schema table; key = the host-resolved schema type name (`RouteEntry.schema_name`), value = a JSON-Schema-subset schema. **Mutually exclusive with `schemas_file`**; setting both fails create. |
| `schemas_file` | string | no | — | Read the schema table from a file (read from disk and parsed at create; must be a JSON object). Mutually exclusive with `schemas`. |
| `require_schema` | boolean | no | `false` | When `true`, routes without a configured schema fail immediately with `protocol.schema_not_found`; by default they pass through. |
| `on_violation` | string | no | `reject` | `reject` fails on validation failure; `warn` logs a host WARN and passes through. Only `reject`/`warn` are accepted. |

Validation timing: decode after `from_msgpack`, encode before `to_msgpack`. The validator reuses the host's minimal JSON-Schema-subset validator (conventions in Protocol Codec Plugins).

**Difference from json**: msgpack has no `{"payload": ...}` envelope unwrapping — both schema validation and what is delivered to Lua are the entire `from_msgpack` result.

### Complete app.yaml Example

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: protocol.msgpack.gateway
      package: protocol.msgpack
      required: true
      config:
        schemas:
          auth.LoginRequest:
            type: object
            required: [userid]
            properties:
              userid: { type: string, minLength: 1 }
            additionalProperties: false
        require_schema: false
        on_violation: reject
  bindings:
    protocol.msgpack: protocol.msgpack.gateway

actors:
  - name: gateway
    script: scripts/gateway.lua
    network:
      tcp: "0.0.0.0:8001"
      protocol:
        name: game.msgpack
        envelope:
          type: idlen
          route_id_bytes: 4
          length_bytes: 4
          endian: big
        body:
          codec: msgpack
          provider: protocol.msgpack
        routing:
          source: header.route_id
          unknown_route_action: drop
        routes:
          - id: 1001
            name: auth.LoginRequest
            action: decode
            lazy_decode: false
```

`body.codec` must match the instance capability (`msgpack`); `provider` is the binding logical name, not the instance ID.

## Interface Contract

Implements `shield_protocol_codec_v1` defined in [`include/shield/plugin/protocol_codec.h`](https://github.com/cuihairu/shield/blob/main/include/shield/plugin/protocol_codec.h), with `codec_name = "msgpack"`. Addressing is isomorphic to json: the host resolves the route into the final schema type name and passes it in as `route_name`; the plugin does a single-level lookup.

- **decode**: `from_msgpack` decodes the payload bytes (empty/corrupt bytes fail) → optional schema validation → dumped to canonical JSON and handed to the host.
- **encode**: parse the canonical JSON message → optional schema validation → `to_msgpack` produces the bytes.

## Usage Example

Same tightening sequence as json: observe first, then reject.

```yaml
config:
  schemas_file: "conf/payload-schemas.json"
  on_violation: warn   # observe mode: violations only log WARN (prefix protocol.msgpack) and pass through as usual
```

The `request` the Lua handler receives is the entire decoded message (no envelope):

```lua
-- routes: { id = 1001, name = "auth.LoginRequest", action = "decode" }
function handler(ctx, client, request)
    return { ok = true, userid = request.userid }
end
```

## Error Handling

| Error code | Trigger | Propagation |
|--------|----------|------|
| `protocol.schema_not_found` | `require_schema: true` and the route has no configured schema | same as decode/encode failure |
| `protocol.decode_failed` | payload not MessagePack / reject on violation (c2s) | session `decode_error` → **disconnect** |
| `protocol.encode_failed` | message unparseable / reject on violation (s2c) | that outbound message is dropped, no disconnect |
| `protocol.codec_unavailable` | provider not configured, instance not started, or codec name mismatch | listener fails to build at startup |

Config shape errors at create time (mutually exclusive keys, type errors, `schemas_file` unreadable / not a JSON object) report `plugin.create.failed`.

## Known Boundaries

- No envelope unwrapping: the top-level `{"payload": ...}` convention exists only on the json path.
- Lua egress does not automatically call the plugin's encode (see Protocol Codec Plugins Known Limitations).
- Body-route protocols (`route_key`) cannot configure an external provider — a build-time error; intended for header routing or single-route profiles.
- The codec is locked per listener; per-route / per-session negotiation is not supported.

## Related Links

- Protocol Codec Plugins — codec plugin boundaries, validator keyword subset, error mapping
- protocol.json — the JSON version of the same schema validation
- Plugin System — manifest, bootstrap pipeline, binding resolution
