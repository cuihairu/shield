# Xmldef Compiler / Runtime MVP

> Status: draft / implementation-structure spec.
>
> This document clarifies one more layer of where `xmldef` Phase 1 code lives: which modules belong to the compiler toolchain, which to the runtime descriptor layer, which to the transport integration layer, and which to the Lua binding compile layer. The goal is that subsequent implementations no longer revolve around one "big miscellaneous xmldef codec" but are split along clean boundaries.

For the Phase 1 scope and deliverables, read the Xmldef Phase 1 Implementation Plan first. For the descriptor field contract, read the [Xmldef Descriptor Spec](xmldef-descriptor-spec.md).

## Goal

The Phase 1 implementation structure must guarantee four things:

1. XML interpretation happens only in the toolchain, never on the runtime hot path.
2. The runtime depends only on the compiled descriptor, never on XML files.
3. `XmldefBodyCodec` handles only codec semantics; it does not also manage bindings, manifests, or code generation.
4. Lua handler lookup is compiled at startup; run time does only `route_id` hits.

One addition: the `XmldefBodyCodec` here is "the runtime adaptation name convenient for the current Phase 1"; it does not mean `xmldef` can only bind a single native binary representation. In the long-term model, `xmldef` leans toward contract source / descriptor provider, and can later drive multiple payload codecs.

## Layer Split

Recommended split into these four layers:

```text
xmldef xml
  -> compiler toolchain
  -> descriptor artifacts
  -> runtime descriptor layer
  -> transport/lua integration
```

Corresponding responsibilities:

- compiler toolchain: read manifest/XML, do semantic validation, build the canonical IR, output descriptors
- descriptor artifacts: `descriptor.debug.json`, `route_constants.json`, `manifest.json`
- runtime descriptor layer: load the descriptor and build query indexes
- transport/lua integration: `XmldefBodyCodec`, the protocol pipeline adapter, the Lua binding registry

## Recommended Source Layout

Based on the repo's current structure, recommended placement:

```text
tools/xmldef_compiler/
  main.cpp
  manifest_loader.hpp/.cpp
  xml_loader.hpp/.cpp
  semantic_validator.hpp/.cpp
  canonical_ir.hpp/.cpp
  descriptor_json_writer.hpp/.cpp
  route_constants_writer.hpp/.cpp
  manifest_writer.hpp/.cpp

include/shield/transport/xmldef/
  descriptor_registry.hpp
  schema_types.hpp
  xmldef_codec.hpp
  lua_binding_registry.hpp

src/transport/xmldef/
  descriptor_registry.cpp
  xmldef_codec.cpp
  lua_binding_registry.cpp

tests/transport/
  test_xmldef_descriptor_registry.cpp
  test_xmldef_codec.cpp
  test_xmldef_lua_binding_registry.cpp

tests/integration/
  test_xmldef_gateway_flow.cpp
```

### Why Under `transport`

Phase 1's runtime `xmldef` still serves the fixed protocol pipeline, so placing it in the `shield_transport`
subdirectory first is recommended.

But note:

- this is the runtime-side descriptor/codec/binding layer
- not the compiler toolchain
- not the Unity/U3D generator

The toolchain should not be stuffed into `src/transport/`.

## Compiler MVP Modules

### 1. ManifestLoader

Responsibilities:

- read `protocol/manifest.xml`
- parse package metadata
- collect the module list
- produce the stable input of "which XML files still need loading"

Not responsible for:

- schema semantic validation
- type/method number assignment
- descriptor output

Suggested minimal interface:

```text
load_manifest(path) -> ManifestModel
```

### 2. XmlLoader

Responsibilities:

- load module XML
- parse message/type/enum/field raw syntax trees
- keep source locations for error reporting

Not responsible for:

- business semantic judgment
- ID stability policy

Suggested minimal interface:

```text
load_modules(manifest) -> RawModuleAst[]
```

### 3. SemanticValidator

Responsibilities:

- cross-file type resolution
- method/type/field/enum uniqueness checks
- direction/kind legality checks
- client-visible reference boundary checks
- reserved ID conflict checks

This is the critical gate of the compiler MVP.
Any XML not satisfying the descriptor contract should fail fast here.

Suggested minimal interface:

```text
validate(raw_modules) -> ValidatedSchemaModel
```

### 4. CanonicalIrBuilder

Responsibilities:

- generate stable module/type/method/route records
- normalize `direction`
- normalize `kind`
- build the type_ref tree
- do the canonical sort
- compute hash inputs

The IR produced here is the true input of the writers and generators.

Suggested minimal interface:

```text
build_ir(validated_model) -> CanonicalPackage
```

### 5. DescriptorJsonWriter

Responsibilities:

- output `descriptor.debug.json`
- ensure field names, field order, and shape are stable
- lose none of the fields generators/runtime need

This layer should not re-derive semantics; it only serializes the IR.

### 6. RouteConstantsWriter

Responsibilities:

- output the simplified route table from the canonical IR
- convenient for frontend generators, script tools, and tests to consume directly

### 7. ManifestWriter

Responsibilities:

- output the build artifact manifest
- record package/version/hash/generated file list

It is not part of the schema semantics itself, but it is part of the Phase 1 deliverables.

## Runtime MVP Modules

### 1. DescriptorRegistry

Responsibilities:

- load `descriptor.debug.json`
- build query indexes
- provide the method/type/route lookup API

Recommended internal indexes:

- `route_id -> MethodDescriptor`
- `full_name -> MethodDescriptor`
- `type_id -> TypeDescriptor`
- `method_id -> MethodDescriptor`

Suggested minimal interface:

```text
load_descriptor(path) -> DescriptorRegistry
find_method_by_route(route_id)
find_method_by_name(full_name)
find_type(type_id)
find_request_type(route_id)
find_response_type(route_id)
find_binding_hint(route_id)
```

`DescriptorRegistry` is not responsible for:

- Lua function lookup
- bytes encode/decode
- XML fallback

### 2. SchemaTypes

Although Phase 1 only uses the debug JSON, it is still recommended to extract the runtime's internal
type model separately instead of passing JSON objects around raw.

Suggested abstractions:

- `TypeDescriptor`
- `StructDescriptor`
- `FieldDescriptor`
- `EnumDescriptor`
- `MethodDescriptor`
- `RouteDescriptor`
- `TypeRef`

That way, later switching to `descriptor.bin` requires no interface changes in `XmldefBodyCodec` or the
Lua binding layer.

### 3. XmldefBodyCodec

Responsibilities:

- look up the method schema by `route_id`
- decode bytes -> structured business message
- encode business message -> bytes

It depends on:

- `DescriptorRegistry`
- `MethodDescriptor`
- `TypeDescriptor`

It is not responsible for:

- route policy
- route extraction
- Lua dispatch
- XML file reading

Recommended internal split:

- `decode_scalar`
- `decode_enum`
- `decode_struct`
- `decode_list`
- `encode_scalar`
- `encode_enum`
- `encode_struct`
- `encode_list`

This way, adding `map` later extends naturally.

### 4. LuaBindingRegistry

Responsibilities:

- read the binding table exposed by the Lua service
- validate against descriptor methods at startup
- compile `route_id -> cached lua function`

Suggested interface:

```text
compile_bindings(descriptor_registry, lua_module) -> LuaBindingRegistry
find_handler(route_id) -> cached_function
```

Run-time dispatch should do only:

```text
route_id -> cached_function
```

It should not do:

- `full_name` string lookups
- per-packet reflective `module[handler_name]`
- dynamic function name guessing

## Transport Integration

The way `xmldef` plugs into the fixed protocol pipeline should be very narrow:

```text
Envelope
  -> RouteExtractor
  -> RoutePolicy
  -> XmldefBodyCodec.decode
  -> LuaBindingRegistry dispatch
```

Outbound:

```text
Lua business message
  -> RouteResolver
  -> XmldefBodyCodec.encode
  -> Envelope
```

This means the `xmldef` runtime is responsible only for the `BodyCodec` slot and the Lua dispatch slot.
It should not re-invent a gateway middleware.

## Startup Order

Phase 1's recommended startup order:

1. read `descriptor.debug.json`
2. build the `DescriptorRegistry`
3. initialize `XmldefBodyCodec`
4. load the Lua gateway service module
5. compile the `LuaBindingRegistry`
6. validate all required bindings
7. begin receiving client traffic

The key points:

- the descriptor compiles before Lua bindings
- Lua bindings compile before sessions receive packets
- missing bindings are a startup failure, not a weak runtime warning

## Validation Split

Two error classes must be distinguished:

### Compiler-time Errors

- route/type/field/enum ID conflicts
- type references that don't exist
- illegal direction/kind
- client-visible referencing server-only

These are schema contract errors and must be reported by the compiler.

### Startup-time Errors

- descriptor file missing
- descriptor file format version incompatible
- Lua binding missing
- binding pointing at a nonexistent function

These are deployment/wiring errors and must be reported at runtime startup.

### Runtime Errors

- decode bytes failure
- illegal enum value
- missing required field
- egress message shape not matching the schema

These are request-level errors and must be returned or logged as transport/protocol errors.

## Test Split

Tests should be split per layer, not one giant integration test:

### Compiler Tests

- manifest load
- module load
- semantic validation failure cases
- canonical ordering stability
- `descriptor.debug.json` golden tests

### Runtime Unit Tests

- `DescriptorRegistry` lookups
- `XmldefBodyCodec` scalar/enum/struct/list encode/decode
- `LuaBindingRegistry` compile/validation

### Integration Tests

- protocol pipeline ingress decode-local -> Lua handler
- Lua s2c helper -> xmldef encode -> envelope (route_id comes only from the descriptor, never inferred from the body)
- missing bindings cause startup failure

## Phase 1 Non-goals

This document does not require Phase 1 to do these directly:

- `descriptor.bin` runtime load
- the Unity/U3D generator implementation
- Unreal/TS generators
- XML hot reload
- schema patch / diff
- entity/component runtime

These either belong to later phases or to a separate tooling plugin.
