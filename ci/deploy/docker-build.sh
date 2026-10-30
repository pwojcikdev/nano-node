#!/bin/bash
set -euo pipefail

# Builds the node image for one network and hands its local name and registry name to the push steps.
# NETWORK picks BETA or LIVE, CI_TAG is the build tag baked into the binaries.
# A LIVE build of anything but a release tag is a nightly, published under its own name.

tag=${CI_TAG:?CI_TAG must name the build tag}

case "${NETWORK:-}" in
    BETA)
        network=beta
        name=nano-beta
        ;;
    LIVE)
        network=live
        if [[ "${tag}" =~ ^V[0-9]+\.[0-9]+$ ]]; then
            name=nano
        else
            name=nano-nightly
        fi
        ;;
    *)
        echo "NETWORK must be BETA or LIVE, got '${NETWORK:-}'" >&2
        exit 1
        ;;
esac

image="nano-node:${tag}"

docker build \
    --file docker/node/Dockerfile \
    --tag "${image}" \
    --build-arg NANO_NETWORK="${network}" \
    --build-arg CI_TAG="${tag}" \
    --build-arg REPOSITORY="${GITHUB_REPOSITORY:-nanocurrency/nano-node}" \
    .

echo "Built ${image} for publishing as ${name}"
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    {
        echo "image=${image}"
        echo "name=${name}"
    } >>"${GITHUB_OUTPUT}"
fi
