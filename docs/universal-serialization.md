# Universal Serialization

This page is a legacy design reference from the pre-refactor architecture.

Serialization is not a standalone Shield core module in the current refactor
boundary. Only the minimum encoding needed by `net`, `transport`, `data`, and
Lua bindings should remain in the runtime core.

## Current Refactor Decision

- Do not expose a broad universal serialization framework as core API.
- Do not make Protobuf / MessagePack / custom serializer registration part of
  the initial Lua-first runtime contract.
- Keep payload representation simple until the `shield.*` Lua API is stable.
- Revisit schema and binary protocol tooling later as optional extensions.

## Historical Notes

The previous architecture explored JSON, Protobuf, MessagePack, type traits, and
format auto-detection. Those ideas may still be useful for future protocol or
schema work, but they must not expand the current runtime boundary.

For current design decisions, read:

- [Architecture Design](architecture.md)
- [API Notes](api.md)
- [Roadmap](roadmap.md)
