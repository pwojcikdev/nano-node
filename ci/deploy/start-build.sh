#!/bin/bash
set -euo pipefail

# Starts Build & Deploy on a build tag, unless the tag already has a run.
# Usage: ci/deploy/start-build.sh <tag>
# GITHUB_REPOSITORY names the repository, else gh uses the checkout's origin; gh takes its token from GH_TOKEN.

die() {
    echo "::error::$*" >&2
    exit 1
}

tag=${1:-}
if [[ -z "${tag}" ]]; then
    die "Usage: $0 <tag>"
fi

has_run() {
    (($(gh run list ${GITHUB_REPOSITORY:+--repo "${GITHUB_REPOSITORY}"} --workflow build_deploy.yml --branch "${tag}" --limit 1 --json databaseId --jq length) > 0))
}

# Checked before every attempt: a dispatch whose reply was lost has still started a run
for attempt in 1 2 3; do
    if ((attempt > 1)); then
        sleep 15
    fi
    if has_run; then
        echo "Build & Deploy already has a run for ${tag}"
        exit 0
    fi
    if gh workflow run ${GITHUB_REPOSITORY:+--repo "${GITHUB_REPOSITORY}"} build_deploy.yml --ref "${tag}"; then
        echo "::notice::Started Build & Deploy on ${tag}"
        exit 0
    fi
    echo "Starting Build & Deploy on ${tag} failed, attempt ${attempt} of 3"
done

die "Could not start Build & Deploy on ${tag}"
