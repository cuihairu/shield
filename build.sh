#!/usr/bin/env bash
# build.sh — Build and run Shield in one command.
# Usage:
#   ./build.sh           # Debug build
#   ./build.sh release   # Release build
#   ./build.sh run       # Build + run (default config, echo listener on :7900)
#   ./build.sh clean     # Clean build directory

set -euo pipefail

BUILD_TYPE="Debug"
RUN_AFTER=false
CLEAN=false

if [[ "${1:-}" == "release" ]]; then
    BUILD_TYPE="Release"
elif [[ "${1:-}" == "run" ]]; then
    RUN_AFTER=true
elif [[ "${1:-}" == "clean" ]]; then
    CLEAN=true
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

if $CLEAN; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
    echo "Done."
    exit 0
fi

# ---------------------------------------------------------------------------
# Preflight: fail with actionable messages instead of raw toolchain errors.
# ---------------------------------------------------------------------------
preflight_fail() {  # preflight_fail <what> <how-to-fix>
    echo "ERROR: $1" >&2
    echo "  fix: $2" >&2
    exit 1
}

command -v cmake >/dev/null 2>&1 \
    || preflight_fail "cmake not found" \
          "install CMake >= 3.30 (e.g. 'sudo apt install cmake' or https://cmake.org/download/)"
CMAKE_MAJOR_MINOR="$(cmake --version | head -1 | sed 's/[^0-9.]*//;s/[^0-9.].*//')"
preflight_ok() {
    awk -v v="$CMAKE_MAJOR_MINOR" -v r="3.30" \
        'BEGIN { split(v,a,"."); split(r,b,"."); exit !(a[1]>b[1] || (a[1]==b[1] && a[2]>=b[2])) }'
}
preflight_ok \
    || preflight_fail "cmake ${CMAKE_MAJOR_MINOR} is too old (need >= 3.30)" \
          "upgrade CMake (pip install --user cmake>=3.30 works too)"
command -v ninja >/dev/null 2>&1 \
    || echo "note: ninja not found — cmake will use its default generator (slower)"

CXX="${CXX:-}"
if [[ -z "$CXX" ]]; then
    for CAND in g++ clang++; do
        command -v "$CAND" >/dev/null 2>&1 && CXX="$CAND" && break
    done
fi
[[ -n "$CXX" ]] \
    || preflight_fail "no C++ compiler found (looked for g++/clang++)" \
          "install gcc >= 13 or clang >= 17 (both provide C++23)"
echo 'int main(){}' | "$CXX" -std=c++23 -x c++ - -fsyntax-only >/dev/null 2>&1 \
    || preflight_fail "$CXX does not accept -std=c++23" \
          "use gcc >= 13 or clang >= 17 (check '$CXX --version')"

TOOLCHAIN=""
if [[ -n "${VCPKG_ROOT:-}" ]]; then
    TOOLCHAIN="-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
else
    echo "note: VCPKG_ROOT not set — skipping the vcpkg toolchain file."
    echo "      first build needs it (deps are compiled from source); see docs/quickstart.md"
fi

echo "=== Shield Build ==="
echo "Build type: $BUILD_TYPE"
echo "Build dir:  $BUILD_DIR"
echo ""

# Configure
cmake -B "$BUILD_DIR" \
    $TOOLCHAIN \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DSHIELD_BUILD_TESTS=ON \
    -DSHIELD_BUILD_EXAMPLES=ON

# Build
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" -j "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo ""
echo "=== Build Complete ==="
echo "Binary: $BUILD_DIR/bin/shield"
echo ""

# Run
if $RUN_AFTER; then
    echo "=== Starting Shield Server (echo listener on 0.0.0.0:7900) ==="
    echo "=== in another terminal: python3 scripts/client_demo.py ==="
    "$BUILD_DIR/bin/shield" --config config/app.yaml
fi
