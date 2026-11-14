// [SHIELD_DEVCORE] Read-only "Lua script" face: merge /ops/services with
// per-service /ops/services/:name snapshots (the engine records the script
// path for config-declared runtime actors) and resolve the recorded path
// against the workspace roots so the editor can jump to it. Hot reload is
// an existing console capability and is deliberately NOT here — this face
// only lists and opens.

import { isAbsolute, normalize, resolve } from 'node:path';

import type { LuaServiceDetail, OpsHttpClient } from './opsHttp.js';

export interface LuaServiceEntry extends LuaServiceDetail {
  /// Best-effort absolute path for the recorded script, or null when the
  /// engine did not record one (spawned services have no config script).
  resolvedPath: string | null;
}

/// The engine records `script` as given in the actor config: absolute when
/// the config wrote an absolute path, otherwise config-dir-relative
/// (resolve_script_path in src/lua/lua_service.cpp tries CWD then the
/// config source dir). devcore receives the raw value over HTTP, so it
/// resolves candidates against the workspace roots the IDE knows about.
export function resolveScriptPath(
  script: string | undefined,
  roots: readonly string[],
): string | null {
  if (!script || script.length === 0) {
    return null;
  }
  if (isAbsolute(script)) {
    return normalize(script);
  }
  // Config-relative scripts legitimately reference sibling directories of
  // the config dir ("../scripts/auth.lua"), so resolve without requiring
  // the result to stay under the root; first root wins.
  for (const root of roots) {
    if (root.length === 0) {
      continue;
    }
    return normalize(resolve(root, script));
  }
  return null;
}

export class LuaServicesSource {
  private readonly client: OpsHttpClient;
  private readonly roots: () => readonly string[];

  constructor(client: OpsHttpClient, roots: readonly string[] | (() => readonly string[])) {
    this.client = client;
    this.roots = typeof roots === 'function' ? roots : () => roots;
  }

  async refresh(): Promise<LuaServiceEntry[]> {
    const details = await this.client.allServiceDetails();
    const roots = this.roots();
    return details.map((detail) => ({
      ...detail,
      resolvedPath: resolveScriptPath(detail.script, roots),
    }));
  }
}
