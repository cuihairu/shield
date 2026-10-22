# Quick Start

Goal: **a new user goes from clone to a real client RPC round-trip within 10 minutes**.
Below is a linear path where every step has an expected result; when something breaks, check "Common Problems" at the end first.

## 0. Prerequisites (within 5 minutes)

| Dependency | Version | Check command |
| --- | --- | --- |
| CMake | ≥ 3.30 | `cmake --version` |
| C++ compiler | gcc ≥ 13 / clang ≥ 17 / MSVC 19.38+ | `g++ --version` |
| vcpkg | any recent version | `echo $VCPKG_ROOT` |
| python3 | ≥ 3.6 (for the client demo) | `python3 --version` |

If you don't have vcpkg:

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT=~/vcpkg
```

Ubuntu missing base tools: `sudo apt install -y build-essential cmake ninja-build
curl zip unzip tar pkg-config python3`.

`./build.sh` runs the same environment preflight at the start and prints a one-line fix hint for whatever is missing.

## 1. Build (20–40 minutes the first time)

```bash
./build.sh release
```

The first build has vcpkg compile dependencies from source (boost/CAF/openssl/lua etc.); that is where the time goes. After that, incremental builds take only tens of seconds. To share build artifacts across machines/clones:

```bash
export VCPKG_DEFAULT_BINARY_CACHE=~/.cache/vcpkg-binary
mkdir -p $VCPKG_DEFAULT_BINARY_CACHE
```

## 2. Start the default services

```bash
./build/bin/shield --config config/app.yaml
```

Seeing `echo started` means success. The default config provides:

| Capability | Address | Notes |
| --- | --- | --- |
| echo game listener | `0.0.0.0:7900` | unauthenticated c2s `echo` → s2c `echo_result` |
| HTTP ops plane | `127.0.0.1:8080` | `curl 127.0.0.1:8080/ops/health` |
| console | `/tmp/shield-console.sock` | `help` lists all commands |

## 3. Connection check (open a new terminal)

```bash
python3 scripts/client_demo.py
```

Expected output (route 100 = `echo_result`, data echoed back as-is):

```text
reply route=100 body={"seq":1,"data":{"hello":"shield"},"time_ms":...}
```

Frame format (idlen framing): `[route_id:2B big-endian][length:2B big-endian][JSON body]`;
the body is pure business JSON with no routing fields — `scripts/client_demo.py` is under
100 lines total and can serve directly as a client protocol reference.

## 4. Generate your own project (one command)

```bash
./scripts/new_project.sh ~/my_game
./build/bin/shield --config ~/my_game/config/app.yaml &
python3 scripts/client_demo.py --port 8100
```

Scaffold contents: `config/app.yaml` (echo listener on 8100) + `scripts/echo.lua` +
a README. Generation runs the `--check-config` self-check automatically.

## 5. Advanced: the full client RPC loop

`examples/hello_world/` is the complete example with auth/binding/forwarding (auth → player →
room). Build it with:

```bash
cmake -B build-examples -S . -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Release -DSHIELD_BUILD_TESTS=OFF -DSHIELD_BUILD_EXAMPLES=ON
cmake --build build-examples --target hello_world
./build-examples/bin/hello_world --config examples/hello_world/config/app.yaml
python3 scripts/client_demo.py --port 8001 --message '{"player_id":"p1"}'
```

The login reply is route 100 `login_result`. The example ships a real TCP e2e test
(`tests/acceptance/test_client_rpc_e2e.cpp`).

## 6. Turn it into your own gameplay

1. Declare routes in `actors[].rpc.routes` (ids globally unique; one c2s and one s2c entry each);
2. Write the same-named handler in the Lua script: `function M.<binding>(ctx, client, req)`;
   reply with `shield.client_rpc.<s2c_name>(client, table)`;
3. Validate offline with `shield --check-config --config <your config>` before starting;
4. Use `shield.client.bind` when authentication needs to switch the target service
   (hello_world's auth.lua is the shortest example).

For the Lua-side capabilities (send/call/sleep/timer/fork/config/log…), see the
Lua API Contract; for configuration semantics see Runtime Config Semantics.

## Common Problems

- **vcpkg fails to download dependencies (SSL/early EOF)**: network jitter; just rerun
  `./build.sh release` (already-downloaded packages hit the cache); setting
  `VCPKG_DEFAULT_BINARY_CACHE` is even more robust.
- **Port 7900/8001 occupied**: change `network.tcp` in the config and start again.
- **cmake/compiler too old**: see the version requirements in step 0; `pip install --user
  "cmake>=3.30"` is the shortcut to a new cmake without touching the system install.
- **Windows**: use `build.bat` (or open the CMake project directly in VS); vcpkg is likewise
  provided via `VCPKG_ROOT`; adjust path examples to Windows style.

## Design Goal and Current State

The end goal is that users need only one C++ entry point (`shield::run(argc, argv)`) plus one
`app.yaml`; the `shield` binary and scaffold above are the minimal implementation of that shape.
Config errors fail fast at startup; `--check-config` validates offline; a Lua service file
must return a table; `shield.call`/`shield.sleep`/timers/client RPC handlers are all
coroutine-based paths where you may sleep/call without blocking the actor.
