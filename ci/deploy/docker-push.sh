#!/bin/bash
set -euo pipefail

# Pushes a local image to a registry repository under the given tags and reports the digest it has there.
# Usage: ci/deploy/docker-push.sh <image> <repository> <tag>...
# The caller logs in to the registry first.

die() {
    echo "::error::$*" >&2
    exit 1
}

if (($# < 3)); then
    die "Usage: $0 <image> <repository> <tag>..."
fi

image=$1
repository=$2
shift 2

# Three attempts; a later attempt skips the layers an earlier one already uploaded
push() {
    local reference=$1
    local attempt
    for attempt in 1 2 3; do
        if docker push "${reference}"; then
            return 0
        fi
        echo "Pushing ${reference} failed, attempt ${attempt} of 3"
        if ((attempt < 3)); then
            sleep 15
        fi
    done
    return 1
}

echo "Pushing ${image} to ${repository} as: $*"
for tag in "$@"; do
    docker tag "${image}" "${repository}:${tag}"
    push "${repository}:${tag}" || die "Could not push ${repository}:${tag}"
done

# Docker records Docker Hub repositories without their docker.io/ prefix
digest=$(docker image inspect --format '{{range .RepoDigests}}{{println .}}{{end}}' "${image}" | grep -F "${repository#docker.io/}@" | head -n 1 | cut -d@ -f2)
if [[ -z "${digest}" ]]; then
    die "No digest recorded for ${repository}"
fi

echo "::notice::Pushed ${repository}@${digest}"
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    echo "digest=${digest}" >>"${GITHUB_OUTPUT}"
fi
