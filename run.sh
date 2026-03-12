#!/bin/bash
# Build mGBA for 3DS and serve the .cia over HTTP.
#
# Usage: ./run.sh [--clean] [build_dir] [port]
#   --clean    Wipe build dir and re-run cmake
#   build_dir  Default: build
#   port       Default: 8080

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Parse args — pass through to build.sh, grab port for serve.py
BUILD_ARGS=()
PORT=""
BUILD_DIR=""

for arg in "$@"; do
  case "$arg" in
    --clean) BUILD_ARGS+=("--clean") ;;
    [0-9][0-9][0-9][0-9]*) PORT="$arg" ;;
    *) BUILD_DIR="$arg"; BUILD_ARGS+=("$arg") ;;
  esac
done

BUILD_DIR="${BUILD_DIR:-build}"

# Build
"$SCRIPT_DIR/build.sh" "${BUILD_ARGS[@]}"

# Serve
echo "=== Serve ==="
SERVE_ARGS=("$SCRIPT_DIR/$BUILD_DIR")
[ -n "$PORT" ] && SERVE_ARGS+=("$PORT")
python "$SCRIPT_DIR/serve.py" "${SERVE_ARGS[@]}"
