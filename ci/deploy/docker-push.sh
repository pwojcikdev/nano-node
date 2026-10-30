#!/bin/bash
set -euo pipefail

# Pushes a local image to a registry repository under the given tags and reports the digest it has there.
# Usage: ci/deploy/docker-push.sh <image> <repository> <tag>...
# The caller logs in to the registry first.

if (($# < 3)); then
    echo "Usage: $0 <image> <repository> <tag>..." >&2
    exit 1
fi

image=$1
repository=$2
shift 2

for tag in "$@"; do
    docker tag "${image}" "${repository}:${tag}"
    docker push "${repository}:${tag}"
done

# Docker records Docker Hub repositories without their docker.io/ prefix
digest=$(docker image inspect --format '{{range .RepoDigests}}{{println .}}{{end}}' "${image}" | grep -F "${repository#docker.io/}@" | head -n 1 | cut -d@ -f2)
if [[ -z "${digest}" ]]; then
    echo "::error::No digest recorded for ${repository}" >&2
    exit 1
fi

echo "::notice::${repository} ${digest}"
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    echo "digest=${digest}" >>"${GITHUB_OUTPUT}"
fi
