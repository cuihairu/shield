#!/usr/bin/env bash
# build.sh — Build and run Shield in one command.
# Usage:
#   ./build.sh           # Debug build
#   ./build.sh release   # Release build
#   ./build.sh run       # Build + run
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

# Configure
TOOLCHAIN=""
if [[ -n "${VCPKG_ROOT:-}" ]]; then
    TOOLCHAIN="-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
fi

echo "=== Shield Build ==="
echo "Build type: $BUILD_TYPE"
echo "Build dir:  $BUILD_DIR"
echo ""

cmake -B "$BUILD_DIR" \
    $TOOLCHAIN \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
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
    echo "=== Starting Shield Server ==="
    "$BUILD_DIR/bin/shield" server --config config/app.yaml
fi
