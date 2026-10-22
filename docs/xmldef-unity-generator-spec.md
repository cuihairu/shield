# Xmldef Unity Generator Spec

> Status: draft / deferred tooling spec.
>
> This document defines the inputs, outputs, interface, code organization, and constraints of the Unity / U3D `xmldef` generator plugin. The goal is that given the same descriptor, the client team can stably generate C# DTOs, route constants, a gateway facade, and handler stubs instead of hand-writing protocols.

## Scope

This document only defines the Unity/U3D generator plugin:

- the input descriptor contract
- the generator plugin interface
- the output file set
- generated-code naming and layout
- the runtime facade boundary

Not defined:

- Unity network transport implementation details
- Web / Unreal generators
- the server-side runtime `XmldefBodyCodec`
- editor UI experience

If your current concern is which descriptor outputs to freeze first, the minimal runtime execution surface, and the compiler MVP scope, read together with the Xmldef Phase 1 Implementation Plan.
If your current concern is the server runtime and compiler module boundaries, read together with the [Xmldef Compiler / Runtime MVP](xmldef-compiler-runtime-mvp.md).

## Inputs

Minimal generator inputs:

- `descriptor.bin`
- `descriptor.debug.json`
- `route_constants.json`
- generator options

The generator is recommended to mainly consume `descriptor.debug.json`, with `descriptor.bin` as the consistency-check source.

### Required Descriptor Fields

The Unity generator depends on these descriptor fields:

- package id
- package version
- code namespace
- method full name
- route id
- schema name
- direction
- request type
- response type
- item type
- field layout
- enum definitions

Missing any one should fail fast directly.

## Plugin Contract

### Positioning

This is a **tooling plugin**, not a runtime `shield_plugin`.

Reasons:

- it runs at development time, not on the server host
- it targets Unity project artifact generation
- it should not join `shield_plugin`'s instance/binding/ABI lifecycle

### Minimal Interface

Suggested minimal interface:

```text
generator_plugin {
  name: string
  version: string
  target: "unity-csharp"
  generate(input, output_dir, options) -> result
}
```

`input` contains at least:

- descriptor paths
- package metadata

`result` contains at least:

- the generated file list
- warnings
- errors

### Generator Options

Suggested minimal options:

- `namespace_override`
- `output_root`
- `runtime_namespace`
- `dto_style` (`class` / `struct`)
- `emit_handler_interface` (`true/false`)
- `emit_partial_stubs` (`true/false`)
- `emit_route_constants` (`true/false`)

## Output Layout

Suggested output directory:

```text
generated/csharp/
  Game.Protocol/
    RouteIds.cs
    XmldefCodec.cs
    GatewayClient.cs
    IXmldefHandler.cs
    Messages/
      Avatar/
        MoveRequest.cs
      Login/
        ReqAuth.cs
    Handlers/
      XmldefHandlerStub.cs
```

The generator must guarantee:

- stable file paths
- stable namespaces
- repeatable output when regenerating from the same descriptor

## Generated Artifacts

### 1. RouteIds.cs

Purpose:

- provide stable route constants
- keep the business layer from hand-writing magic numbers

Sketch:

```csharp
namespace Game.Protocol {
    public static class RouteIds {
        public const uint AvatarMove = 0x1001;
        public const uint LoginReqAuth = 0x1002;
    }
}
```

### 2. DTO Types

Each request / response / item type generates a C# DTO.

Sketch:

```csharp
namespace Game.Protocol.Avatar {
    public sealed class MoveRequest {
        public int EntityId { get; set; }
        public float X { get; set; }
        public float Y { get; set; }
        public float Z { get; set; }
    }
}
```

Rules:

- generate a `class` by default, unless the generator option specifies `struct`
- field names map stably from schema names
- enums generate standalone C# enums
- list/map map to stable .NET collection types

### 3. XmldefCodec.cs

Purpose:

- wrap descriptor-driven encode/decode
- hide raw route/schema operations from upper layers

Suggested minimal interface:

```csharp
public interface IXmldefCodec {
    ArraySegment<byte> Encode(uint routeId, object message);
    object Decode(uint routeId, ReadOnlySpan<byte> payload);
}
```

If the client runtime ends up being a descriptor-driven dynamic codec, this file may also just be a typed facade.

### 4. GatewayClient.cs

Purpose:

- provide a typed send/call API
- keep the business layer from handling route ids directly

Sketch:

```csharp
public sealed class GatewayClient {
    public void SendAvatarMove(Game.Protocol.Avatar.MoveRequest req) { ... }
    public Task<Game.Protocol.Login.ReqAuthReply> CallLoginReqAuthAsync(
        Game.Protocol.Login.ReqAuthRequest req) { ... }
}
```

Rules:

- `send` -> synchronous or fire-and-forget API
- `call` -> `Task<T>`
- `stream` -> `IAsyncEnumerable<T>` or a callback wrapper; extensible later

### 5. IXmldefHandler.cs

Purpose:

- provide a strongly-typed handler interface for client inbound dispatch

Sketch:

```csharp
public interface IXmldefHandler {
    void OnAvatarMove(Game.Protocol.Avatar.MoveRequest msg);
    void OnLoginNotice(Game.Protocol.Login.Notice msg);
}
```

If a method's direction does not allow client reception, that method must not enter the handler interface.

### 6. Optional Partial Stubs

To lower integration cost, it may generate:

```csharp
public partial class GatewayClient {
}
```

or:

```csharp
public partial class XmldefHandlerBase : IXmldefHandler {
    public virtual void OnAvatarMove(MoveRequest msg) {}
}
```

The generator must not overwrite user-written partial files.

## Naming Rules

### Namespace

The default namespace comes from the descriptor's `code_namespace`.

For example:

```text
code_namespace = "Game.Protocol"
```

generates:

- `Game.Protocol`
- `Game.Protocol.Avatar`
- `Game.Protocol.Login`

### Method Name Mapping

`Avatar.move` maps to:

- route constant: `AvatarMove`
- send method: `SendAvatarMove`
- handler method: `OnAvatarMove`

### Field Name Mapping

If the schema uses `snake_case`, the generator may map to PascalCase:

- `entity_id` -> `EntityId`
- `room_id` -> `RoomId`

This rule must be stable and documented; it must not vary within one project.

## Direction Mapping

The generator must obey the descriptor's `direction`:

- `client_to_server`: generate the send API, not a client inbound handler
- `server_to_client`: generate the client handler, not a proactive send API
- `bidirectional`: generate both
- `server_to_server`: ignored by default by the Unity generator

## Threading Boundary

One of Unity/U3D's key constraints is the main-thread model.

The client facade generated by the generator should therefore layer explicitly:

- transport / socket callback thread
- main-thread dispatch adapter
- user handler

The recommendation is not to hard-code "force switch to main thread" in the DTO or codec, but to reserve it at the facade layer:

```csharp
public interface IMainThreadDispatcher {
    void Post(Action action);
}
```

Whether to switch to the main thread is decided by the Unity runtime adapter, not by the descriptor or the DTO.

## Versioning

Generated code should retain the descriptor metadata:

- package id
- package version
- schema root hash

Suggested generation:

```csharp
public static class XmldefSchemaInfo {
    public const string PackageId = "game";
    public const string Version = "1.0.0";
    public const string SchemaRootHash = "...";
}
```

Used for handshakes, self-checks, and logging.

## Failure Rules

The generator must fail fast in these cases:

- the descriptor is missing required fields
- duplicate route ids
- method name collisions after mapping
- field name collisions after mapping
- unsupported types present
- server-only types requested for client export

Silent skipping is not allowed.

## Non-goals

This document explicitly does not:

- directly generate socket library implementations
- directly generate Unity Editor panels
- expose `route_id` directly to the business layer
- make clients hand-write binary encode/decode
- hook the Unity generator into the server plugin host

## Recommended Delivery Order

Suggested implementation order:

1. Freeze the [Xmldef Descriptor Spec](xmldef-descriptor-spec.md) first
2. Then implement the minimal Unity generator
3. First produce `RouteIds.cs` + DTOs + `GatewayClient.cs`
4. Then add the handler interface / partial stubs
5. Finally do the more complex main-thread dispatch adaptation

Rationale:

- route constants + DTOs + facade already eliminate most hand-written protocols
- main-thread policy and concrete network library adaptation can be deferred

## Success Criteria

If implemented correctly, Unity business code should look close to:

```csharp
client.SendAvatarMove(new MoveRequest {
    EntityId = 7,
    X = 10,
    Y = 20,
    Z = 30,
});
```

and not:

```csharp
var bytes = ManualEncode(...);
socket.Send(bytes);
```

That is the minimum value line of the `xmldef` generator.
