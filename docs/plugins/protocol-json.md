# protocol.json

> Optional validation stand-in for the built-in JSON codec: when not enabled the built-in json keeps working as-is; when enabled you gain per-route payload schema validation.

`protocol.json` is one of the official `shield.protocol.codec.v1` providers, with codec name `json`. **Without `body.provider` set, the core's built-in json keeps working with zero behavior change**; after explicitly binding the provider to this plugin, the decode/encode semantics are identical to the built-in (including the optional top-level `{"payload": ...}` envelope unwrapping), with the added ability to validate payload shapes per route at the ingress/egress boundary.

**Selection at a glance**: JSON has the best readability, zero debugging cost, and no extra protocol runtime — the first choice for development and config-style protocols; for binary size pick protocol.msgpack; for cross-language strongly-typed schemas pick protocol.protobuf; for flatc toolchain integration pick protocol.flatbuffers.

## Package Information

- **Package ID**: `protocol.json`
- **Interface**: `shield.protocol.codec.v1` (capabilities: `json`)
- **Version**: 1.0.0
- **CMake option**: `SHIELD_BUILD_PLUGIN_JSON`
- **Source**: `plugins/protocol.json/`
- **Dependencies**: [nlohmann/json](https://github.com/nlohmann/json) (header-only, already in the vcpkg manifest with the core; no separate runtime library)

## Build Enable

Turn on `SHIELD_BUILD_PLUGIN_JSON` at the CMake configure stage:

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_JSON=ON
cmake --build build
```

The artifact outputs to `plugins/protocol.json/bin/libshield_protocol_json.{so|dylib|dll}`. This plugin introduces no new third-party port (the codec reuses the nlohmann/json already installed with the core). CI's plugins-ci workflow builds with `SHIELD_BUILD_PLUGIN_JSON=ON`.

## Config Schema

The instance config (`plugins.instances[].config`) has four keys, all optional:

| Field | Type | Required | Default | Description |
|------|------|------|--------|------|
| `schemas` | object | no | — | Inline schema table; key = the host-resolved schema type name (`RouteEntry.schema_name`, i.e. the route name or the route's `request_schema` override), value = a JSON-Schema-subset schema. **Mutually exclusive with `schemas_file`**; setting both fails create. |
| `schemas_file` | string | no | — | Read the schema table from a file (read from disk and parsed at create; must be a JSON object, each value also an object). Mutually exclusive with `schemas`. |
| `require_schema` | boolean | no | `false` | When `true`, routes without a configured schema fail immediately with `protocol.schema_not_found`; by default they pass (no schema = no constraints). |
| `on_violation` | string | no | `reject` | `reject` fails on validation failure; `warn` logs a host WARN and passes (observe mode). Only `reject`/`warn` are accepted; other values fail create. |

Schema validation reuses the host's minimal JSON-Schema-subset validator (keyword set, token-level semantics of `integer`, `additionalProperties` enforced only as boolean false, etc. — conventions in Protocol Codec Plugins); the plugin carries no second implementation. Schema shape errors surface as `schema evaluation error` decode/encode failures and never escape the C ABI.

### Complete app.yaml Example

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: protocol.json.gateway
      package: protocol.json
      required: true
      config:
        schemas:
          auth.LoginRequest:
            type: object
            required: [userid]
            properties:
              userid: { type: string, minLength: 1 }
            additionalProperties: false
        on_violation: reject
  bindings:
    protocol.json: protocol.json.gateway   # binding logical name -> instance ID

actors:
  - name: gateway
    script: scripts/gateway.lua
    network:
      tcp: "0.0.0.0:8001"
      protocol:
        name: game.json
        envelope:
          type: idlen
          route_id_bytes: 4
          length_bytes: 4
          endian: big
        body:
          codec: json
          provider: protocol.json          # references the binding logical name above
        routing:
          source: header.route_id
          unknown_route_action: drop
        routes:
          - id: 1001
            name: auth.LoginRequest
            action: decode
            lazy_decode: false
```

`body.codec` must match the instance capability (`json`); `provider` is the binding logical name, not the instance ID — swapping the instance the provider points to requires no actor config change.

## Interface Contract

Implements `shield_protocol_codec_v1` defined in [`include/shield/plugin/protocol_codec.h`](https://github.com/cuihairu/shield/blob/main/include/shield/plugin/protocol_codec.h), with `codec_name = "json"`. Both decode/encode are single-level name lookups: at compile time the host resolves the route into the final schema type name (the `request_schema` override or the route name itself) and passes it in as `route_name`.

### decode (c2s)

1. `nlohmann::json::parse` parses the payload bytes (empty ranges/unparseable bytes fail).
2. Unwrap the optional top-level `{"payload": ...}` envelope — **schema validation applies to the unwrapped business message** (same semantics as the built-in json codec; the shape delivered to Lua is exactly the shape the schema describes).
3. If a schema exists, validate; `reject` on violation returns `protocol.decode_failed`; `warn` on violation logs WARN and passes.
4. After passing, the business message is dumped to canonical JSON and handed to the host as the Lua handler's request table.

### encode (s2c)

1. Parse the canonical JSON message input.
2. Validate against the schema looked up by `route_name` (same as above, no envelope wrapping).
3. After passing, dump to JSON bytes as the outbound payload.

## Usage Example

A c2s (reject) schema violation **disconnects** (reusing the decode-failure semantics, `error_code_ = "decode_error"`); an s2c (encode) violation only drops that outbound message. Before tightening in production, start with observe mode:

```yaml
config:
  schemas_file: "conf/payload-schemas.json"
  require_schema: false      # routes without a declared schema pass as usual
  on_violation: warn         # only log to observe production violations, then tighten to reject
```

Violation details (including the path of the first error point, e.g. `userid: ...`) go to the host log with the prefix `protocol.json`.

The `request` the Lua handler receives is already the decoded, validated business message — no envelope handling needed:

```lua
-- routes: { id = 1001, name = "auth.LoginRequest", action = "decode" }
function handler(ctx, client, request)
    -- request = { userid = "..." } (the schema guarantees presence and non-emptiness)
    return { ok = true, userid = request.userid }
end
```

## Error Handling

| Error code | Trigger | Propagation |
|--------|----------|------|
| `protocol.schema_not_found` | `require_schema: true` and the route has no configured schema | same as decode/encode failure |
| `protocol.decode_failed` | payload unparseable / reject on violation (c2s) | session `decode_error` → **disconnect** |
| `protocol.encode_failed` | message unparseable / reject on violation (s2c) | that outbound message is dropped, no disconnect |
| `protocol.codec_unavailable` | provider not configured, instance not started, or codec name mismatch | listener fails to build at startup |

Config shape errors at instance startup (create) — mutually exclusive keys, type errors, `schemas_file` unreadable / not a JSON object — report `plugin.create.failed` and bootstrap fails.

## Known Boundaries

- Lua egress does not automatically call the plugin's encode: the inbound decode result becomes the request table directly; proactive business outbound still needs explicit encoding then `shield.client_rpc` (see Known Limitations).
- Body route-key extraction (`route_key`) cannot configure an external provider: a body-route protocol configured with `protocol.json` is a build-time error; this validation plugin targets header routing (idlen/typelen) or single-route profiles.
- The codec is locked per listener (`actors[].network.protocol.body`); per-route / per-session negotiation is not supported.

## Related Links

- Protocol Codec Plugins — codec plugin boundaries, schema addressing convergence, validator keyword subset
- Protocol Routing Design — route table, envelope, routing source
- Plugin System — manifest, bootstrap pipeline, binding resolution
- Lua API — handler binding and the `shield.client_rpc` outbound convention
