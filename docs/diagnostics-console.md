# Interactive Diagnostics Console

## Overview

shield ships an interactive diagnostics console for live observation and diagnosis of running services. The console has two layers:

- **Root layer (C++)**: observes framework-internal state (plugins, configuration, cluster, logs, the Lua service list)
- **Script layer (Lua)**: attaches to a running service's Lua VM and executes arbitrary Lua code (similar to IPython)

The transport layer uses a Unix Domain Socket (downgraded to TCP loopback on Windows) with a line protocol (newline-delimited JSON).

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  CLI Client (replxx)                                │
│  shield-console [--sock path]                       │
└──────────────┬──────────────────────────────────────┘
               │ Unix Domain Socket (newline-delimited JSON)
               ▼
┌─────────────────────────────────────────────────────┐
│  ConsoleServer (boost::asio::local::stream_protocol)│
│  ┌─────────────┐  ┌─────────────┐                   │
│  │ConsoleSession│  │ConsoleSession│  ...             │
│  └──────┬──────┘  └──────┬──────┘                   │
│         └────────┬───────┘                          │
│           CommandDispatcher                          │
│     ┌────────────┼────────────┐                     │
│     ▼            ▼            ▼                     │
│  RootCommands  LuaCommands  HelpHandler             │
│  (C++ queries) (Lua REPL)   (help)                  │
└─────────────────────────────────────────────────────┘
```

## Configuration

Enable in `config.yaml`:

```yaml
# Unix socket console
console:
  enabled: true
  socket_path: /tmp/shield-console.sock   # Unix socket path

# HTTP endpoints (optional)
http:
  enabled: true
  host: "0.0.0.0"        # listen address
  port: 8080             # listen port
```

After startup, connect with `shield-console`:

```bash
shield-console --sock /tmp/shield-console.sock
```

Or use the HTTP endpoints:

```bash
# get status
curl http://localhost:8080/ops/status

# list services
curl http://localhost:8080/ops/services

# execute Lua code
curl -X POST http://localhost:8080/ops/eval \
  -H "Content-Type: application/json" \
  -d '{"code": "return shield.self()"}'
```

## Command Reference

### Command Mode

| Command | Description |
|------|------|
| `help` | list all available commands |
| `root.status` | service/plugin/cluster overview |
| `root.services` | list all Lua services |
| `root.service <name>` | service details |
| `root.plugins` | plugin package/instance/binding list |
| `root.plugin <id>` | plugin details (including last_error) |
| `root.config [key]` | config query (no argument = full JSON) |
| `root.cluster` | cluster node status |
| `root.server` | server state (state/uptime/version/watcher count; prompts unavailable when the server module is not enabled) |
| `root.log.level [level]` | read/set the log level (debug/info/warn/error) |
| `attach <service>` | enter the service's Lua VM interactive REPL |
| `eval <code>` | run one-shot code in an isolated Lua sandbox |
| `exit` / `quit` | disconnect |

### Lua REPL Mode (after attach)

```
shield> attach gameserver1
lua:gameserver1> return shield.self()
"gameserver1"
lua:gameserver1> local x = 10
lua:gameserver1> return x * 2
20
lua:gameserver1> for i=1,3 do print(i) end
1
2
3
lua:gameserver1> if x > 5 then
...   print("yes")
... end
yes
lua:gameserver1> detach
```

| Command | Description |
|------|------|
| `detach` | exit the Lua REPL, back to command mode |
| `exit` | same as detach |
| any Lua code | executes in the service's VM; multi-line statements supported |

**Features**:
- Multi-line input: an incomplete statement (e.g. `if ... then`) automatically waits for the next line, with the prompt turning into `...`
- Persistent state: `local` variables survive for the session
- Timeout protection: each execution has a 5-second timeout, preventing infinite loops from blocking the worker

## Response Protocol

One JSON object per line; the `type` field identifies the kind:

| type | Description | Example |
|------|------|------|
| `result` | command execution result | `{"type":"result","data":42}` |
| `error` | error | `{"type":"error","message":"Service not found"}` |
| `attached` | attached to a service | `{"type":"attached","service":"gameserver1"}` |
| `detached` | detached | `{"type":"detached"}` |
| `continue` | multi-line input, waiting for more lines | `{"type":"continue"}` |
| `output` | Lua print() output | `{"type":"output","text":"hello"}` |

## HTTP API Reference

### Endpoint List

| Method | Path | Description |
|------|------|------|
| GET | `/ops/status` | service/plugin/cluster/server state |
| GET | `/ops/services` | list all Lua services |
| GET | `/ops/plugins` | list all plugins |
| GET | `/ops/config?key=<key>` | query configuration |
| POST | `/ops/eval` | execute Lua code |

### Response Format

All endpoints return JSON with the same structure as the console protocol:

```json
{
  "type": "result",
  "data": { ... }
}
```

Or an error:

```json
{
  "type": "error",
  "message": "error description"
}
```

### Examples

```bash
# get status
curl http://localhost:8080/ops/status
# {"type":"result","data":{"services":["gateway","player"],"plugins":[...],"server":{"state":"running","uptime_seconds":3600,...}}}
# the server block appears only when SHIELD_ENABLE_SERVER is compiled in and enabled (read-only snapshot, see runtime-server.md)

# list services
curl http://localhost:8080/ops/services
# {"type":"result","data":["gateway","player"]}

# query configuration
curl http://localhost:8080/ops/config?key=database.host
# {"type":"result","data":"localhost"}

# execute Lua code
curl -X POST http://localhost:8080/ops/eval \
  -H "Content-Type: application/json" \
  -d '{"code": "return 1 + 2"}'
# {"type":"result","data":3}
```

## Threading Model

- **Thread-safe subsystems** (PluginHost, Config, ClusterManager, ServerManager, Logger): the console thread calls them directly
- **Service actor subsystems**: dispatched through CAF/service actor scheduling to the corresponding runtime owner, with results returned via `promise/future`
- Actor scheduling latency is usually negligible

## Implementation Files

### Transport layer
- `include/shield/net/console_session.hpp` / `src/net/console_session.cpp`
- `include/shield/net/console_server.hpp` / `src/net/console_server.cpp`
- `include/shield/net/http_server.hpp` / `src/net/http_server.cpp`

### Command layer
- `include/shield/console/command_dispatcher.hpp` / `src/console/command_dispatcher.cpp`
- `include/shield/console/root_commands.hpp` / `src/console/root_commands.cpp`
- `include/shield/console/lua_commands.hpp` / `src/console/lua_commands.cpp`
- `include/shield/console/cluster_status.hpp` / `src/console/cluster_status.cpp`
- `include/shield/console/server_status.hpp` / `src/console/server_status.cpp`
- `include/shield/console/ops_http_handler.hpp` / `src/console/ops_http_handler.cpp`

### CLI client
- `tools/shield-console/main.cpp`

### Tests
- `tests/net/test_console_server.cpp`

### Dependency changes
- `include/shield/lua/lua_service.hpp` — added the `exec_lua()` method
- `include/shield/lua/lua_runtime.hpp` — added the `exec_lua()` method
- `src/bootstrap/bootstrap.cpp` — GlobalState gained console_server/http_server with lifecycle management
- `vcpkg.json` — added the `replxx` dependency
