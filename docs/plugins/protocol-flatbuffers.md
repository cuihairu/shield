# protocol.flatbuffers

> FlatBuffers codec plugin: driven by `.fbs` text schemas, binary ↔ JSON bridging, direct access to the flatc toolchain ecosystem.

`protocol.flatbuffers` is one of the official `shield.protocol.codec.v1` providers, with codec name `flatbuffers`. On create it loads the `.fbs` text schema from disk; decode converts FlatBuffers binary to JSON (`GenerateText`), encode parses JSON into FlatBuffers binary per the schema. Enable by binding this plugin through `body.provider`.

**Selection at a glance**: the value lies in flatc toolchain integration (`.fbs` goes straight into the FlatBuffers ecosystem) and single-schema strongly-typed encode; compared with protocol.protobuf there is no descriptor pre-compilation (this implementation re-parses the schema text on every call and bridges via JSON — not a zero-copy path), so stress-test throughput for high-traffic scenarios; if you just want to get started quickly, pick protocol.json / protocol.msgpack.

## Package Information

- **Package ID**: `protocol.flatbuffers`
- **Interface**: `shield.protocol.codec.v1` (capabilities: `flatbuffers`, `fbs`)
- **Version**: 1.0.0
- **CMake option**: `SHIELD_BUILD_PLUGIN_FLATBUFFERS`
- **Source**: `plugins/protocol.flatbuffers/`
- **Dependencies**: [FlatBuffers](https://flatbuffers.dev/) (vcpkg port `flatbuffers`, `flatbuffers::flatbuffers` CMake target) + nlohmann/json

## Build Enable

Turn on `SHIELD_BUILD_PLUGIN_FLATBUFFERS` at the CMake configure stage:

```bash
cmake -B build -DSHIELD_BUILD_PLUGIN_FLATBUFFERS=ON
cmake --build build
```

This option triggers two things:

1. The shared library under `plugins/protocol.flatbuffers/` is built, output to `plugins/protocol.flatbuffers/bin/`.
2. CMake passes the vcpkg manifest feature `protocol-flatbuffers` through to vcpkg, which installs the `flatbuffers` port automatically.

CI's plugins-ci workflow builds with `SHIELD_BUILD_PLUGIN_FLATBUFFERS=ON`.

## Config Schema

The instance config has two keys, **at least one must be set**; if both are missing, create fails (`plugin.config.invalid`):

| Field | Type | Required | Default | Description |
|------|------|------|--------|------|
| `schema` | string | one of the two | — | Path to the `.fbs` schema file (e.g. `conf/game.fbs`). For multi-file projects or projects with `include`, specify files explicitly or pass them merged. |
| `schema_dir` | string | one of the two | — | Directory containing `.fbs` files. On create the directory is scanned and the **first enumerated** `.fbs` file is taken — no enumeration order is guaranteed; with multiple `.fbs` files in a directory the result is indeterminate, so only use this key for single-schema directories. |

Behavioral details (performance-related, stated as-is):

- The schema text is read and cached **at create time** (unreadable file / missing directory / no `.fbs` in the directory → create fails).
- But every decode/encode **re-runs `Parser::Parse`** over the schema text — with a large schema and high traffic this is significant overhead; include schema parsing in your stress-test cost.
- This plugin does **not** have protocol.json/msgpack's per-route payload schema validation (`schemas` / `require_schema` / `on_violation` do not exist); constraints come from the `.fbs` itself: encode JSON that does not match the schema fails to parse.

### Complete app.yaml Example

```yaml
plugins:
  directory: "./plugins"
  instances:
    - id: protocol.flatbuffers.game
      package: protocol.flatbuffers
      required: true
      config:
        schema: "conf/game.fbs"
  bindings:
    protocol.flatbuffers: protocol.flatbuffers.game

actors:
  - name: gateway
    script: scripts/gateway.lua
    network:
      tcp: "0.0.0.0:8001"
      protocol:
        name: game.flatbuffers
        envelope:
          type: idlen
          route_id_bytes: 4
          length_bytes: 4
          endian: big
        body:
          codec: flatbuffers
          provider: protocol.flatbuffers
        routing:
          source: header.route_id
          unknown_route_action: drop
        routes:
          - id: 1001
            name: auth.LoginRequest
            action: decode
            lazy_decode: false
```

`body.codec` must match the instance capability (`flatbuffers`); `provider` is the binding logical name, not the instance ID.

## Interface Contract

Implements `shield_protocol_codec_v1` defined in [`include/shield/plugin/protocol_codec.h`](https://github.com/cuihairu/shield/blob/main/include/shield/plugin/protocol_codec.h), with `codec_name = "flatbuffers"`.

- **decode**: an empty payload returns `"{}"` directly; otherwise a fresh `Parser` is created and `Parse`d with the cached schema text, then `GenerateText` converts the binary to JSON text delivered to the host.
- **encode**: the canonical JSON message input is first parsed per the schema with `Parser::Parse` (this step also performs schema validation — JSON not matching the `.fbs` definitions fails), then the buffer is taken from the builder as the outbound bytes.

The root type defined in the `.fbs` determines the target message for encode; in a `.fbs` with multiple messages, the route-to-message mapping is still carried by the host's `route_name` (schema type name) single-level addressing convention.

## Usage Example

The client serializes requests with code generated by flatc; the server-side Lua handler receives the JSON-bridged table:

```lua
-- routes: { id = 1001, name = "auth.LoginRequest", action = "decode" }
function handler(ctx, client, request)
    -- request is generated by GenerateText; field names match the .fbs declarations
    return { ok = true, userid = request.userid }
end
```

During debugging you can first use `flatc --json` to verify the `.fbs` schema parses (the plugin's create uses the same parser; schema syntax errors surface at create time as `failed to parse FlatBuffers schema`).

## Error Handling

| Error code | Trigger | Propagation |
|--------|----------|------|
| `protocol.decode_failed` | schema text parse failure / binary-to-JSON failure (c2s) | session `decode_error` → **disconnect** |
| `protocol.encode_failed` | schema parse failure / JSON not matching the schema / buffer acquisition failure (s2c) | that outbound message is dropped, no disconnect |
| `plugin.config.invalid` (create) | both `schema` and `schema_dir` missing, unreadable file, missing directory, no `.fbs` in directory | bootstrap fails at startup |
| `protocol.codec_unavailable` | provider not configured, instance not started, or codec name mismatch | listener fails to build at startup |

## Known Boundaries

- JSON bridging implementation: both decode/encode go through JSON text, and the schema is re-parsed on every call — zero-copy is a capability of the FlatBuffers library itself; this plugin's bridging path does not enjoy it (see Protocol Codec Plugins Known Limitations).
- The multi-file directory semantics of `schema_dir` are indeterminate (takes the first enumerated `.fbs`); production configs should use `schema` to point at files explicitly.
- Lua egress does not automatically call the plugin's encode; body-route protocols (`route_key`) cannot configure an external provider.
- The codec is locked per listener; per-route / per-session negotiation is not supported.

## Related Links

- Protocol Codec Plugins — codec plugin boundaries, ABI shape, Known Limitations
- [FlatBuffers official documentation](https://flatbuffers.dev/flatbuffers_guide_writing_schema.html) — `.fbs` schema syntax
- Plugin System — manifest, bootstrap pipeline, binding resolution
