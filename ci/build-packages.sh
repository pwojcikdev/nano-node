#!/bin/bash
set -euo pipefail

# Builds the release packages for one network and collects them in packages/.
# NETWORK picks BETA or LIVE, CI_TAG is the build tag the packages are named after.

case "${NETWORK:-}" in
    BETA)
        network=beta
        build_type=RelWithDebInfo
        ;;
    LIVE)
        network=live
        build_type=Release
        ;;
    *)
        echo "NETWORK must be BETA or LIVE, got '${NETWORK:-}'" >&2
        exit 1
        ;;
esac

tag=${CI_TAG:?CI_TAG must name the build tag}
script_dir=$(dirname "${BASH_SOURCE[0]}")

# The Windows installer bundles the Visual C++ runtime, which the install rules expect in the build directory
if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
    mkdir -p build
    curl -fsSL -o build/vc_redist.x64.exe https://aka.ms/vs/17/release/vc_redist.x64.exe
fi

NANO_NETWORK=${network} \
    BUILD_TYPE=${build_type} \
    NANO_GUI=ON \
    CI_TAG=${tag} \
    "${script_dir}/build.sh"

# Run the cpack that ships with cmake: the Windows runners also have Chocolatey's cpack on the PATH
cpack="$(dirname "$(command -v cmake)")/cpack"
(cd build && "${cpack}" -C "${build_type}")

mkdir -p packages
mv build/nano-node-"${tag}"-* packages/

echo "Packages:"
ls -l packages
