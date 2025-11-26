#!/bin/bash

# Build script for nano-node project
# Usage: ./ai_build.sh [--release]

set -e

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Default to debug build
BUILD_TYPE="debug"
BUILD_DIR="${SCRIPT_DIR}/cmake-build-debug"

# Parse arguments
for arg in "$@"; do
    case $arg in
        --release)
            BUILD_TYPE="release"
            BUILD_DIR="${SCRIPT_DIR}/cmake-build-release"
            shift
            ;;
    esac
done

# Detect number of CPUs (use performance cores only on macOS)
if [[ "$OSTYPE" == "darwin"* ]]; then
    # Get number of performance cores only (not efficiency cores)
    NCPU=$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || sysctl -n hw.ncpu)
else
    NCPU=$(nproc)
fi

echo "Building nano-node (${BUILD_TYPE} mode)..."
echo "Build directory: ${BUILD_DIR}"
echo "Using ${NCPU} parallel jobs"
echo ""

# Run cmake build and filter output
# Remove lines that are long compiler invocations (starting with /usr/bin/c++)
cmake --build "${BUILD_DIR}" --target all -j "${NCPU}" 2>&1 | \
    sed '/^\/usr\/bin\/c++ .*-o.*-c /d'

# Preserve the exit code from cmake
exit ${PIPESTATUS[0]:-0}
