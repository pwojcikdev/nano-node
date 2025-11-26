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
# Replace long compiler and linker invocations with shortened versions
cmake --build "${BUILD_DIR}" --target all -j "${NCPU}" 2>&1 | \
    sed 's|^/usr/bin/c++ .*-o \([^ ]*\) -c \([^ ]*\)$|/usr/bin/c++ ... -o \1 -c \2|' | \
    sed 's|^: && /usr/bin/c++ .* -o \([^ ]*\) .*&&.*$|: \&\& /usr/bin/c++ ... -o \1 ... \&\& :|'

# Preserve the exit code from cmake
exit ${PIPESTATUS[0]:-0}
