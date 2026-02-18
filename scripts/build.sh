#!/bin/bash
# Build mGBA for 3DS using devkitPro toolchain.
#
# Works from any shell on Windows (VS Code, Git Bash, MSYS2) and Linux/macOS.
# Auto-detects devkitPro if environment is not already set.
#
# Usage: ./build.sh [--clean] [build_dir]
#   --clean   Wipe build dir and re-run cmake (default on first build)
#   Default build_dir: mgba/build

set -e

CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --clean) CLEAN=1 ;;
    *) BUILD_DIR="$arg" ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-mgba/build}"

# Resolve to absolute path relative to script dir
case "$BUILD_DIR" in
  /*) ;;
  *) BUILD_DIR="$SCRIPT_DIR/$BUILD_DIR" ;;
esac

SRC_DIR="$SCRIPT_DIR/mgba"

# --- devkitPro environment setup (Windows auto-detect) ---
if [ -z "$DEVKITPRO" ]; then
  # Try standard Windows install path
  if [ -d "/c/devkitPro" ]; then
    export DEVKITPRO=/c/devkitPro
  elif [ -d "/opt/devkitpro" ]; then
    export DEVKITPRO=/opt/devkitpro
  else
    echo "ERROR: DEVKITPRO is not set and could not find devkitPro installation."
    echo "  Install from: https://devkitpro.org/wiki/Getting_Started"
    echo "  Or set DEVKITPRO manually before running this script."
    exit 1
  fi
  echo "Auto-detected DEVKITPRO=$DEVKITPRO"
fi

if [ -z "$DEVKITARM" ]; then
  export DEVKITARM="$DEVKITPRO/devkitARM"
  echo "Set DEVKITARM=$DEVKITARM"
fi

# Verify the toolchain exists
if [ ! -f "$DEVKITARM/bin/arm-none-eabi-gcc" ] && [ ! -f "$DEVKITARM/bin/arm-none-eabi-gcc.exe" ]; then
  echo "ERROR: arm-none-eabi-gcc not found in $DEVKITARM/bin/"
  echo "  Install devkitARM: (dkp-)pacman -S 3ds-dev"
  exit 1
fi

# Add devkitPro tools to PATH (cmake, make, smdhtool, 3dsxtool, etc.)
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$DEVKITPRO/msys2/usr/bin:$PATH"

# On Windows/MSYS2, the compiler emits Windows paths (C:\...) in dependency
# files which breaks Make's parser. Disabling compiler-generated deps makes
# CMake use its own scanner instead, which handles paths correctly.
CMAKE_EXTRA_ARGS=""
case "$(uname -s)" in
  MSYS*|MINGW*|CYGWIN*)
    CMAKE_EXTRA_ARGS="-DCMAKE_DEPENDS_USE_COMPILER=OFF"
    ;;
esac

echo "=== Environment ==="
echo "  DEVKITPRO=$DEVKITPRO"
echo "  DEVKITARM=$DEVKITARM"
echo "  cmake: $(which cmake 2>/dev/null || echo 'NOT FOUND')"
echo "  make:  $(which make 2>/dev/null || echo 'NOT FOUND')"
echo "  gcc:   $(which arm-none-eabi-gcc 2>/dev/null || echo 'NOT FOUND')"

if [ "$CLEAN" -eq 1 ] || [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  echo "=== Clean ==="
  rm -rf "$BUILD_DIR"
  mkdir -p "$BUILD_DIR"

  echo "=== CMake ==="
  cd "$BUILD_DIR"
  cmake -DCMAKE_TOOLCHAIN_FILE="$SRC_DIR/src/platform/3ds/CMakeToolchain.txt" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        $CMAKE_EXTRA_ARGS \
        "$SRC_DIR" 2>&1
else
  echo "=== Incremental build (use --clean to reconfigure) ==="
  cd "$BUILD_DIR"
fi

echo "=== Build ==="
make -j$(nproc) 2>&1

echo "=== Done ==="
echo "Output: $BUILD_DIR/3ds/mgba.cia"
