#!/usr/bin/env bash
# tools/new_plugin.sh <name> — scaffold a shield plugin package.
#
# Generates plugins/<name>/ with:
#   manifest.yaml      schema v1 manifest (id = <name> with '_' folded to '.')
#   CMakeLists.txt     MODULE library + manifest staging + a POSIX-only ABI
#                      smoke test (dlopen -> get_v1 -> create -> start ->
#                      get_interface -> echo -> shutdown)
#   shield_<name>.cpp  v1 ABI stub providing a working shield.echo.v1 demo
#                      interface (replace with your real interface)
#   <name>.h           the demo interface header (echo vtable)
#   test/test_abi.cpp  the smoke test (Boost.Test)
#
# Also wires the build:
#   - inserts option(SHIELD_BUILD_PLUGIN_<UPPER> ...) before
#     add_subdirectory(plugins) in the root CMakeLists.txt
#   - appends the guarded add_subdirectory(<name>) to plugins/CMakeLists.txt
#
# After scaffolding:
#   cmake -B build -DSHIELD_BUILD_PLUGIN_<UPPER>=ON && \
#   cmake --build build --target test_<name>_abi && \
#   ctest --test-dir build -R test_<name>_abi
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <name>   (lowercase [a-z0-9_], e.g. inventory_events)" >&2
    exit 2
fi

NAME="$1"
if ! echo "$NAME" | grep -qE '^[a-z][a-z0-9_]*$'; then
    echo "error: name must match [a-z][a-z0-9_]* (got: $NAME)" >&2
    exit 2
fi
ID="${NAME//_/.}"
UPPER="$(echo "$NAME" | tr '[:lower:]' '[:upper:]')"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/plugins/$NAME"

if [ -e "$DIR" ]; then
    echo "error: plugins/$NAME already exists" >&2
    exit 1
fi

mkdir -p "$DIR/test"

# --- manifest.yaml ---------------------------------------------------------
cat > "$DIR/manifest.yaml" <<EOF
schema_version: 1
id: $ID
name: $NAME
version: 0.1.0
kind: echo
description: Scaffolded plugin (tools/new_plugin.sh) — replace kind/description
entry: shield_plugin_get_v1
library:
  windows: bin/libshield_$NAME.dll
  linux: bin/libshield_$NAME.so
  macos: bin/libshield_$NAME.dylib
provides:
  - interface: shield.echo.v1
    capabilities:
      - demo
requires: []
config_schema:
  type: object
  properties: {}
EOF

# --- <name>.h : the demo interface header ----------------------------------
cat > "$DIR/$NAME.h" <<EOF
// $ID — demo interface (scaffold). Replace with your real interface
// contract; keep the vtable append-only per docs/plugin-system.md ABI rules.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIELD_${UPPER}_INTERFACE_NAME "shield.echo.v1"

typedef struct shield_${NAME}_echo_v1 {
    uint32_t struct_size;
    // Returns text back to the caller (borrowed pointer, valid until the
    // call returns). Demo only.
    const char* (*echo)(void* self, const char* text);
} shield_${NAME}_echo_v1;

#ifdef __cplusplus
}  // extern "C"
#endif
EOF

# --- shield_<name>.cpp : the v1 ABI stub -----------------------------------
cat > "$DIR/shield_$NAME.cpp" <<EOF
// $ID — scaffolded v1 plugin (tools/new_plugin.sh).
//
// Minimal but complete ABI shape: static abi table -> create() -> instance
// shell (get_interface / start / shutdown / register_lua). The demo
// shield.echo.v1 interface is a working placeholder — swap it for your own.
//
// Plugin rules (docs/plugin-system.md):
//   - plugins are independent shared libraries; do NOT link host statics
//     (shield_net etc.) — talk to the host only through args->host_api
//   - register_lua() must exist on every instance; an empty body (return 0)
//     is the documented no-Lua-surface form
//   - strings crossing the boundary are NULL-terminated UTF-8

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"  // full shield_error_v1 / host_api shapes
#include "$NAME.h"

#include <cstring>
#include <new>
#include <string>

namespace {

// Concrete instance: the v1 shell MUST be the first member (the host only
// ever holds shield_plugin_instance_v1* and the plugin casts back).
struct ${NAME}_instance {
    shield_plugin_instance_v1 shell;
    std::string instance_id;
    std::string echo_buffer;
};

const void* get_interface(shield_plugin_instance_v1* self, const char* name,
                          shield_error_v1* err) {
    auto* inst = reinterpret_cast<${NAME}_instance*>(self);
    if (std::strcmp(name, SHIELD_${UPPER}_INTERFACE_NAME) == 0) {
        static const shield_${NAME}_echo_v1 echo_vt = {
            sizeof(shield_${NAME}_echo_v1),
            [](void* ud, const char* text) -> const char* {
                auto* i = static_cast<${NAME}_instance*>(ud);
                i->echo_buffer = text != nullptr ? text : "";
                return i->echo_buffer.c_str();
            },
        };
        (void)inst;
        return &echo_vt;
    }
    // Not provided is NOT an error: return NULL with err left untouched.
    (void)err;
    return nullptr;
}

int start(shield_plugin_instance_v1* self, shield_error_v1* err) {
    (void)self;
    (void)err;
    return 0;  // open resources here; non-zero + err = startup failure
}

void shutdown(shield_plugin_instance_v1* self) {
    delete reinterpret_cast<${NAME}_instance*>(self);
}

int register_lua(shield_plugin_instance_v1* self, struct lua_State* L,
                 shield_error_v1* err) {
    // No Lua surface yet. To add one: include <sol/sol.hpp>, then
    //   sol::state_view lua(L);
    //   sol::table ns = lua["shield"].get_or_create<sol::table>("$ID");
    //   ns.set_function("ping", [] { return std::string("pong"); });
    (void)self;
    (void)L;
    (void)err;
    return 0;
}

int create(const shield_plugin_create_args_v1* args,
           shield_plugin_instance_v1** out, shield_error_v1* err) {
    auto* inst = new (std::nothrow) ${NAME}_instance();
    if (inst == nullptr) {
        if (err != nullptr) {
            // shield_error_v1 fields are all const char* (see host_api.h).
            err->code = "oom";
            err->message = "instance allocation failed";
            err->phase = "create";
        }
        return 1;
    }
    inst->instance_id = args->instance_id;
    inst->shell.struct_size = sizeof(${NAME}_instance);
    inst->shell.instance_id = inst->instance_id.c_str();
    inst->shell.get_interface = &get_interface;
    inst->shell.start = &start;
    inst->shell.shutdown = &shutdown;
    inst->shell.register_lua = &register_lua;
    // args->config_json is schema-validated with defaults applied;
    // args->host_api / args->ctx are the host callback tables (see
    // include/shield/plugin/host_api.h for config_get / dependency / log).
    *out = &inst->shell;
    return 0;
}

}  // namespace

extern "C" SHIELD_PLUGIN_EXPORT const shield_plugin_abi_v1* shield_plugin_get_v1(
    void) {
    static const shield_plugin_abi_v1 table = {
        SHIELD_PLUGIN_ABI_VERSION,
        sizeof(shield_plugin_abi_v1),
        "$ID",
        "0.1.0",
        &create,
    };
    return &table;
}
EOF

# --- CMakeLists.txt --------------------------------------------------------
cat > "$DIR/CMakeLists.txt" <<EOF
# $ID — scaffolded by tools/new_plugin.sh.
# MODULE library = runtime-loadable plugin (.so/.dylib/.dll), never linked
# into the host. Staged under bin/plugins/$ID/ with the manifest, matching
# the library paths in manifest.yaml (resolved relative to the package root).

add_library(shield_$NAME MODULE shield_$NAME.cpp)

target_include_directories(shield_$NAME
    PRIVATE
        \${PROJECT_SOURCE_DIR}/include
)

set_target_properties(shield_$NAME PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libshield_$NAME"
    LIBRARY_OUTPUT_DIRECTORY "\${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/$ID/bin"
)

file(COPY manifest.yaml "$NAME.h"
     DESTINATION "\${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/$ID")

# ABI smoke test: dlopen the staged module and drive the full instance
# lifecycle (POSIX only; Windows loader wiring is pending, same precedent
# as the scaffold smoke suites).
if(NOT WIN32)
    add_executable(test_${NAME}_abi test/test_abi.cpp)
    target_include_directories(test_${NAME}_abi
        PRIVATE
            \${PROJECT_SOURCE_DIR}/include
            \${PROJECT_SOURCE_DIR}/plugins/$NAME
    )
    find_package(Boost REQUIRED COMPONENTS unit_test_framework headers)
    target_link_libraries(test_${NAME}_abi
        PRIVATE Boost::unit_test_framework Boost::headers \${CMAKE_DL_LIBS})
    # dlopen the staged module: build it as part of the test target.
    add_dependencies(test_${NAME}_abi shield_$NAME)
    add_test(NAME test_${NAME}_abi COMMAND test_${NAME}_abi)
    set_tests_properties(test_${NAME}_abi PROPERTIES
        WORKING_DIRECTORY "\${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        TIMEOUT 60)
endif()

message(STATUS "  + $ID: scaffolded plugin (v1 ABI)")
EOF

# --- test/test_abi.cpp -----------------------------------------------------
cat > "$DIR/test/test_abi.cpp" <<EOF
// $ID ABI smoke test (scaffold): load the staged module and walk the whole
// instance lifecycle through the v1 shell.

#define BOOST_TEST_MODULE ${NAME}_abi

#include <boost/test/unit_test.hpp>

#include <dlfcn.h>

#include <cstring>
#include <string>

#include "shield/plugin/abi.h"
#include "shield/plugin/host_api.h"  // full shield_error_v1 / host_api shapes
#include "$NAME.h"

BOOST_AUTO_TEST_CASE(LoadAndLifecycle) {
    const std::string so = "plugins/$ID/bin/libshield_$NAME.so";
    void* handle = dlopen(so.c_str(), RTLD_NOW);
    BOOST_REQUIRE_MESSAGE(handle != nullptr, dlerror());

    auto get_v1 = reinterpret_cast<const shield_plugin_abi_v1* (*)()>(
        dlsym(handle, "shield_plugin_get_v1"));
    BOOST_REQUIRE(get_v1 != nullptr);

    const shield_plugin_abi_v1* abi = get_v1();
    BOOST_REQUIRE(abi != nullptr);
    BOOST_CHECK_EQUAL(abi->abi_version, SHIELD_PLUGIN_ABI_VERSION);
    BOOST_CHECK_EQUAL(std::string(abi->package_id), "$ID");

    shield_plugin_create_args_v1 args{};
    args.instance_id = "$ID.main";
    args.config_json = "{}";
    shield_plugin_instance_v1* inst = nullptr;
    BOOST_CHECK_EQUAL(abi->create(&args, &inst, nullptr), 0);
    BOOST_REQUIRE(inst != nullptr);
    BOOST_CHECK_EQUAL(std::string(inst->instance_id), "$ID.main");
    BOOST_CHECK_EQUAL(inst->start(inst, nullptr), 0);

    // Demo interface round trip + a not-provided lookup.
    auto* echo = static_cast<const shield_${NAME}_echo_v1*>(
        inst->get_interface(inst, SHIELD_${UPPER}_INTERFACE_NAME, nullptr));
    BOOST_REQUIRE(echo != nullptr);
    BOOST_CHECK_EQUAL(std::string(echo->echo(inst, "ping")), "ping");
    BOOST_CHECK(inst->get_interface(inst, "shield.absent.v1", nullptr) ==
                nullptr);

    inst->shutdown(inst);
    dlclose(handle);
}
EOF

# --- wire into the build ---------------------------------------------------
# The option must be visible when plugins/CMakeLists.txt is evaluated, and
# add_subdirectory(plugins) sits inside a multi-line if(<any plugin option>)
# block — so the new option line goes before that if() opener, not beside the
# add_subdirectory. Walk back from add_subdirectory(plugins) to the innermost
# preceding column-0 if( opener.
ROOT_CMAKE="$ROOT/CMakeLists.txt"
OPTION_NAME="SHIELD_BUILD_PLUGIN_${UPPER}"
if ! grep -q "option($OPTION_NAME" "$ROOT_CMAKE"; then
    LINE="option($OPTION_NAME \"Build the scaffolded $ID plugin\" OFF)"
    python3 - "$ROOT_CMAKE" "$LINE" "$OPTION_NAME" <<'PYEOF'
import re, sys
path, line, opt = sys.argv[1], sys.argv[2], sys.argv[3]
src = open(path).read().split('\n')
idx = next(i for i, l in enumerate(src) if 'add_subdirectory(plugins)' in l)
# walk back to the column-0 if( opener that owns the add_subdirectory
j = idx
while not re.match(r'^if\(', src[j]):
    j -= 1
src.insert(j, line)
# the if() guards add_subdirectory on "any known plugin option" — join the
# OR chain so enabling ONLY the new option still configures the plugins tree
k = j + 1
while not src[k].rstrip().endswith(')'):
    k += 1
cond = src[k].rstrip()
src[k] = cond[:-1] + ' OR'
src.insert(k + 1, '   ' + opt + ')')
open(path, 'w').write('\n'.join(src))
PYEOF
fi

PLUGINS_CMAKE="$ROOT/plugins/CMakeLists.txt"
if ! grep -q "add_subdirectory($NAME)" "$PLUGINS_CMAKE"; then
    {
        echo ""
        echo "if($OPTION_NAME)"
        echo "    add_subdirectory($NAME)"
        echo "endif()"
    } >> "$PLUGINS_CMAKE"
fi

echo "scaffolded plugins/$NAME (package id: $ID)"
echo "next:"
echo "  cmake -B build -D$OPTION_NAME=ON"
echo "  cmake --build build --target test_${NAME}_abi"
echo "  ctest --test-dir build -R test_${NAME}_abi"
