# Xmldef Descriptor Spec

> Status: draft / deferred tooling spec.
>
> This document converges the Xmldef Toolchain Design further into an implementable specification, defining the inputs, validation, IR, outputs, and stability rules of `xmldef/xml -> canonical IR -> descriptor package`. The goal is not to explain "why a descriptor is needed" but to give subsequent compiler/runtime/generator implementations a unified contract.

## Scope

This document only defines:

- the `xmldef/xml` input model
- semantic validation
- the canonical IR
- the descriptor package layout
- ID / hash / stability rules
- the minimal fields runtime consumption needs
- the minimal fields generator consumption needs

Not defined:

- the `shield_protoc` CLI UX
- concrete Unity/U3D generated-code style
- binary encoding details of the runtime `XmldefBodyCodec`
- multi-version patch / Merkle incremental protocols

If your goal is "what does this version do first, and what counts as done", continue with the Xmldef Phase 1 Implementation Plan.
If your goal is "where does code live and how are module responsibilities split", continue with the [Xmldef Compiler / Runtime MVP](xmldef-compiler-runtime-mvp.md).

## Source Model

### Layout

Suggested `xmldef` source file organization:

```text
protocol/
  manifest.xml
  common/
    types.xml
  login/
    messages.xml
  avatar/
    messages.xml
```

`manifest.xml` is the only entry point.

### Manifest

Minimal manifest semantics:

```xml
<protocol-manifest name="game"
                   version="1.0.0"
                   packageId="game"
                   codeNamespace="Game.Protocol">
  <module name="common" path="common"/>
  <module name="login" path="login"/>
  <module name="avatar" path="avatar"/>
</protocol-manifest>
```

Minimal fields:

- `name`
- `version`
- `packageId`
- `codeNamespace`
- `module[]`

Constraints:

- `packageId` stays stable long-term in the descriptor
- `codeNamespace` is not auto-derived from `name`; it must be declared explicitly
- `module.name` is unique within the package

### Message Definition

`xmldef` focuses on message / method / field definitions. Sketch of the minimal message model:

```xml
<protocol name="avatar">
  <message id="0x1001"
           name="Avatar.move"
           direction="client_to_server"
           request="avatar.MoveRequest"
           response=""/>
</protocol>
```

This document does not freeze XML tag name details; it freezes the compiled semantic fields.

Minimal semantic fields:

- `route_id`
- `full_name`
- `direction`
- `request_type`
- `response_type?`
- `target_hint?`
- `binding_hint?`

Schema addressing no longer gets its own field: the schema's location is the type-graph node that
`request_type` points to; when exporting to the host route table, the converged codec ABI produces only
`schema_name` (see the Route section below and the "Schema
addressing convergence" of protocol-codec-plugins.md).

### Type Definition

Reusing the type system conventions of schema-design.md, `xmldef` minimally must support:

- scalar
- enum
- struct
- alias
- list
- map

where map keys are limited in the first version to:

- `string`
- integer scalar
- enum

## Semantic Validation

The compiler must complete strong validation at the XML stage; errors must not be pushed to the runtime.

### Required Checks

Must validate:

- `route_id` uniqueness
- `method full_name` uniqueness
- `request_type` / `response_type` references exist
- field IDs unique within a struct
- field names unique within a struct
- enum value uniqueness
- enum item name uniqueness
- client-visible types must not reference server-only types
- `direction` values legal
- reserved IDs declared in XML must not be reused

### Direction Rules

Minimal supported directions:

- `client_to_server`
- `server_to_client`
- `bidirectional`
- `server_to_server`

The compiler normalizes them into a stable enum and keeps no raw strings.

### Method Kind

`xmldef` must make method kind explicit at the descriptor layer:

- `send`
- `call`
- `stream`

Rules:

- `send` must have a request type and must not have a response/item type
- `call` must have request + response types
- `stream` must have request + item types

## Canonical IR

### Top-level Shape

Minimal canonical IR structure:

```text
package {
  package_id
  package_name
  package_version
  code_namespace
  modules[]
  types[]
  methods[]
  routes[]
}
```

### Package

Fields:

- `package_id: string`
- `package_name: string`
- `package_version: string`
- `code_namespace: string`
- `compiler_version: string`
- `schema_root_hash: bytes[32]`

### Module

Fields:

- `module_id: uint16`
- `name: string`
- `path: string`
- `hash: bytes[32]`
- `dependencies[]: module_id`

Stability rules:

- `module_id` must come from a lock file or an explicit declaration
- must not be reassigned after release

### Type

Fields:

- `type_id: uint32`
- `module_id: uint16`
- `full_name: string`
- `kind: enum`
- `client_visibility: enum`
- `hash: bytes[32]`

Supported `kind`:

- `scalar`
- `enum`
- `struct`
- `alias`

### Struct

Fields:

- `type_id`
- `fields[]`
- `reserved_field_ids[]`

Minimal field fields:

- `field_id: uint32`
- `name: string`
- `type_ref`
- `cardinality`
- `default_value?`
- `deprecated: bool`
- `validation_rules[]`

### TypeRef

Minimal model:

```text
type_ref {
  kind                // scalar | named | list | map
  scalar_kind?
  named_type_id?
  list_item?
  map_key?
  map_value?
}
```

### Method

Minimal fields:

- `method_id: uint16`
- `route_id: uint32`
- `module_id: uint16`
- `full_name: string`
- `short_name: string`
- `kind: enum(send/call/stream)`
- `direction: enum`
- `request_type_id: uint32`
- `response_type_id?: uint32`
- `item_type_id?: uint32`
- `target_hint?: string`
- `binding_hint?: string`
- `hash: bytes[32]`

### Route

The route table is kept explicit, so generators or the runtime don't have to derive it from methods again:

- `route_id: uint32`
- `method_id: uint16`
- `full_name: string`
- `direction`
- `schema_name: string`

`schema_name` is the codec pipeline's only addressing key, resolved at compile time: an explicit
`request_schema` override wins, otherwise `full_name` (the same-name convention). This matches the
convergence rule of the host-side `RouteEntry.schema_name`; routes exported by the catalog produce
only this name and no longer expose numeric addressing such as `schema_id` — such ids, even if they
exist inside a concrete schema system (like protobuf file numbers), are that system's private concept
and enter neither the descriptor's general contract nor the codec ABI.

## Canonical Ordering

To guarantee stable hashes, the IR must be canonically sorted before being written to the descriptor:

- modules by `module_id`
- named types by `type_id`
- methods by `route_id`
- route table by `route_id`
- struct fields by `field_id`
- enum values by `value`

Not allowed:

- outputting in XML file order
- outputting in include order
- sorting by string locale

## Descriptor Outputs

### Required Files

Minimal output:

- `descriptor.bin`
- `descriptor.debug.json`
- `route_constants.json`
- `manifest.json`

### descriptor.bin

For the runtime; requirements:

- fast loading
- little-endian
- stable offsets
- no reliance on target-language reflection

Contains at least:

- package header
- module table
- type table
- method table
- route table
- string table

### descriptor.debug.json

For generators / debug / QA tools.

Minimal requirements:

- able to fully express the binary descriptor's semantic content
- explicit field names
- no loss of `route_id` / `schema_name` / `direction`
- losslessly readable by generators

### route_constants.json

A lightweight artifact for simple tools, scripts, and tests.

Sketch:

```json
{
  "package": "game",
  "version": "1.0.0",
  "routes": [
    {
      "route_id": 4097,
      "name": "Avatar.move",
      "schema_name": "Avatar.move",
      "direction": "client_to_server"
    }
  ]
}
```

## Hashing Rules

### schema_root_hash

`schema_root_hash` must depend only on semantic content, not on:

- compile time
- compiler version
- source dirtiness
- XML formatting
- comments

### meta vs schema

Must distinguish:

- `MetaBlock`: build info; excluded from the semantic hash
- `SchemaBlock`: semantic content; included in the semantic hash

## Runtime Consumption Contract

After loading a descriptor, the runtime can at minimum perform these lookups:

- `find_method_by_route(route_id)`
- `find_method_by_name(full_name)`
- `find_type(type_id)`
- `find_request_type(route_id)`
- `find_response_type(route_id)`

Subsequent `XmldefBodyCodec` implementations must not depend on the XML source files again.

## Generator Consumption Contract

The generator side must at least be able to obtain:

- package id / version / namespace
- route constants
- method full name
- request/response/item type
- direction
- field layout
- optional binding hint

If the descriptor lacks these fields, the compiler contract is deemed incomplete.

## Stability Rules

After release, the following IDs must not be reused:

- `module_id`
- `type_id`
- `method_id`
- `route_id`
- struct `field_id`

Deletion can only:

- mark deprecated
- or move into reserved

They must not be deleted and then reassigned to new semantics.

## Non-goals

This document does not:

- define Unity generated file name details
- define the runtime socket frame format
- define the final API names of the Lua binding table
- define whether the XML tags must be named `<message>` / `<method>`

These belong to upper-layer generator/runtime contracts, not the descriptor compiler's core semantics.
