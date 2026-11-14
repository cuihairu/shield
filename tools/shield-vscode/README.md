# shield-vscode

Thin VS Code shell over [`shield-devcore`](../shield-devcore): status bar
item, DevTools webview panel (CPU/memory curves + live log stream with
filter/search), attach/detach commands, and the read-only Lua script face
(loaded services with file paths, click to open — hot reload stays in the
console; this is only a jump entry point).

## Commands

- `Shield: Attach to Node` — read `shield.*` settings, start the session,
  open the panel.
- `Shield: Detach`
- `Shield: Open DevTools Panel`

## Settings

| setting | meaning |
| --- | --- |
| `shield.opsUrl` | ops HTTP base (`http.host:http.port`, needs `http.enabled=true`) |
| `shield.opsToken` | bearer token (`http.ops_auth_token`) |
| `shield.logFile` | absolute log file path (`log.file.path`, needs `log.file.enabled=true`) |
| `shield.procPid` | optional pid for `/proc` sampling when ops HTTP is off |
| `shield.metricsIntervalMs` | metrics poll cadence (default 2000) |

## Build / package

```sh
pnpm install
pnpm --filter shield-vscode compile   # esbuild bundle -> dist/extension.js
pnpm --filter shield-vscode package   # vsce package -> shield-vscode-0.1.0.vsix
```

`vsce package --no-dependencies` is used: devcore is bundled into
`dist/extension.js` by esbuild, so nothing is pulled from npm at package
time. Do not publish to a marketplace; the version stays 0.1.0.

## Visual Studio route

The core is IDE-agnostic (`tools/shield-devcore`, no VS Code imports). A
future Visual Studio extension either consumes devcore as an npm package or
skips it and talks to the engine HTTP face directly (.NET has first-class
HTTP/JSON). This extension is the VS Code-first shell; no VS project exists
in this repo yet.
