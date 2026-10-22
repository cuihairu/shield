# protocol.protobuf

> Protobuf codec plugin: `FileDescriptorSet`-driven DynamicMessage codec, the first choice for cross-language clients and schema evolution.

`protocol.protobuf` is one of the official `shield.protocol.codec.v1` providers, with codec name `protobuf`. On create it loads the `FileDescriptorSet` descriptor file to build a `DescriptorPool` + `DynamicMessageFactory`, then resolves messages dynamically per the route's type name — business projects do **not** need to generate or link C++ stub code; the server integrates with zero stubs. Enable by binding this plugin through `body.provider`.

**Selection at a glance**: the only choice with a formal schema evolution model (field number compatibility, cross-language code generation); the first pick when clients are multi-language stacks like Unity/UE/Go/Java; if the client is just Lua/JS and you want a quick start, pick protocol.json; for binary size without a schema compiler pick protocol.msgpack; for the flatc ecosystem pick protocol.flatbuffers.

## Package Information

- **Package ID**: `protocol.protobuf`
- **Interface**: `shield.protocol.codec.v1` (capabilities: `protobuf`)
- **Version**: 1.0.0
- **CMake option**: `SHIELD_BUILD_PLUGIN_PROTOBUF`
- **Source**: `plugins/protocol.protobuf/`
- **Dependencies**: [protobuf](https://protobuf.dev/) (vcpkg port `protobuf`, `protobuf::libprotobuf` CMake target, including descriptor/DynamicMessage/json_util) + nlohmann/json

## Build Enable

Turn on `SHIELD_BUILD_PLUGIN_PROTOBUF` at the CMake configure stage:

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_PROTOBUF=ON
cmake --build build
```

This option triggers two things:

1. The shared library under `plugins/protocol.protobuf/` is built, output to `plugins/protocol.protobuf/bin/`.
2. CMake passes the vcpkg manifest feature `protocol-protobuf` through to vcpkg, which installs the `protobuf` port automatically (libprotobuf and protoc included).

CI's ci.yml and plugins-ci workflows both build with `SHIELD_BUILD_PLUGIN_PROTOBUF=ON` (`test_protocol_protobuf_plugin`, including a real descriptor round-trip test).

## Config Schema

The instance config has a single key, **required**:

| Field | Type | Required | Default | Description |
|------|------|------|--------|------|
| `descriptor_set` | string | yes | — | Path to the `FileDescriptorSet` binary file. Missing, non-string, unreadable, or unparseable all report `plugin.config.invalid` at create (bootstrap fails to start; it never runs in a broken state). |

Generate the descriptor file (`--include_imports` embeds the dependent .proto files; always include it):

```bash
protoc --include_imports --descriptor_set_out=conf/game.pb game.proto
```

Descriptors load once at create: `ParseFromString` → add each file to a `SimpleDescriptorDatabase` → build the `DescriptorPool` and `DynamicMessageFactory`. Nothing is read from disk at run time; **after changing a .proto you must restart the process** (the codec vtable is resolved once at listener startup; the plugin cannot be hot-unloaded).

### Complete app.yaml Example

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: protocol.protobuf.game
      package: protocol.protobuf
      required: true
      config:
        descriptor_set: "conf/game.pb"
  bindings:
    protocol.protobuf: protocol.protobuf.game

actors:
  - name: gateway
    script: scripts/gateway.lua
    network:
      tcp: "0.0.0.0:8001"
      protocol:
        name: game.protobuf
        envelope:
          type: idlen
          route_id_bytes: 4
          length_bytes: 4
          endian: big
        body:
          codec: protobuf
          provider: protocol.protobuf
        routing:
          source: header.route_id
          unknown_route_action: drop
        routes:
          - id: 1001
            name: auth.LoginRequest
            action: decode
            lazy_decode: false
          - id: 1002
            name: login
            request_schema: auth.LoginRequest   # explicit override when the type name differs from the route name
            action: decode
            lazy_decode: false
```

`body.codec` must match the instance capability (`protobuf`); `provider` is the binding logical name, not the instance ID. A route's schema type name is resolved by the **same-name convention** (route name = message full name), with `request_schema` as the explicit override — the only escape hatch; there is no id mapping table.

## Interface Contract

Implements `shield_protocol_codec_v1` defined in [`include/shield/plugin/protocol_codec.h`](https://github.com/cuihairu/shield/blob/main/include/shield/plugin/protocol_codec.h), with `codec_name = "protobuf"`. Addressing is single-level: the host resolves the route into the final type name (`RouteEntry.schema_name`) and passes it in as `route_name`; the plugin looks it up directly with `DescriptorPool::FindMessageTypeByName`.

- **decode**: find the descriptor by `route_name` → `DynamicMessage` → `ParseFromArray` (payloads above the int limit are rejected) → `MessageToJsonString` produces canonical JSON handed to the host.
- **encode**: find the descriptor by `route_name` → `JsonStringToMessage` → `SerializeToString` produces the outbound bytes.

**Field name mapping**: the JSON field names output by decode follow protobuf's JSON mapping in **lowerCamelCase** (`user_id` → `userId`); at encode time `JsonStringToMessage` accepts both (lowerCamelCase or the original `.proto` name), but fields read inside Lua handlers are lowerCamelCase.

## Usage Example

The client (any language) serializes requests with protoc-generated stubs; the server-side Lua handler receives the descriptor-driven JSON table:

```lua
-- routes: { id = 1001, name = "auth.LoginRequest", action = "decode" }
function handler(ctx, client, request)
    -- request.userId (lowerCamelCase: proto field user_id via the protobuf JSON mapping)
    return { ok = true, userId = request.userId }
end
```

Scenarios where the req/resp type names differ need no ABI extension: declare one c2s route plus one s2c route each, and outbound encode addresses by the target s2c route's own schema type name (a reply on the same route_id is rejected by the outbound direction check).

## Error Handling

| Error code | Trigger | Propagation |
|--------|----------|------|
| `protocol.schema_not_found` | `route_name` has no matching message in the descriptor pool (both decode/encode look it up) | same as decode/encode failure; startup route table validation also surfaces it |
| `protocol.decode_failed` | payload parse failure / JSON conversion failure (c2s) | session `decode_error` → **disconnect** |
| `protocol.encode_failed` | JSON-to-message failure / serialization failure (s2c) | that outbound message is dropped, no disconnect |
| `plugin.config.invalid` (create) | `descriptor_set` missing / non-string / unreadable / `FileDescriptorSet` parse failure | bootstrap fails at startup |
| `protocol.codec_unavailable` | provider not configured, instance not started, or codec name mismatch | listener fails to build at startup |

## Known Boundaries

- Only binary protobuf payloads are supported; no gRPC, no service-method RPC semantics.
- Lua egress does not automatically call the plugin's encode: the inbound decode result becomes the request table directly; proactive business outbound still needs explicit encoding then `shield.client_rpc` (see Protocol Codec Plugins Known Limitations).
- Body-route protocols (`route_key`) cannot configure an external provider — a build-time error; intended for header routing or single-route profiles.
- The codec is locked per listener; per-route / per-session negotiation is not supported; the descriptor set is read-only at run time — changing a .proto requires a restart.

## Related Links

- Protocol Codec Plugins — codec plugin boundaries, schema addressing convergence (`request_schema` conventions)
- Protocol Routing Design — route table, envelope, routing source
- [protobuf official documentation](https://protobuf.dev/programming-guides/proto3/) — proto3 language guide
- [protobuf JSON mapping](https://protobuf.dev/programming-guides/json/) — lowerCamelCase field name conventions
- Plugin System — manifest, bootstrap pipeline, binding resolution
