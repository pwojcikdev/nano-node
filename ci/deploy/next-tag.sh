#!/bin/bash
set -euo pipefail

# Creates the next build tag for the checked-out branch and hands it to the workflow.
#
# Build tags are V<major>.<minor><suffix><n>: the suffix is DB on develop, RC on the release
# branch of the current major and the branch name elsewhere. With RELEASE=true, accepted only
# on a release branch, the tag is a plain V<major>.<minor>. The version numbers come from
# CMakeLists.txt, which is updated to match and committed first, so the tag carries the version
# it names. The commit of a release is pushed to the branch; a pre-release commit exists only
# through its tag. Nothing is created while the latest tag of the kind already covers HEAD.
#
# Usage: ci/deploy/next-tag.sh [--dry-run]

dry_run=false
if [[ "${1:-}" == "--dry-run" ]]; then
    dry_run=true
fi

release=${RELEASE:-false}
branch=${GITHUB_REF_NAME:-$(git branch --show-current)}
actor=${GITHUB_ACTOR:-$(whoami)}

cmake_version() {
    sed -n "s/^[[:space:]]*set(CPACK_PACKAGE_VERSION_$1 \"\([0-9]*\)\")/\1/p" CMakeLists.txt
}

set_cmake_version() {
    sed -i.bak "s/^\([[:space:]]*\)set(CPACK_PACKAGE_VERSION_$1 \"[0-9]*\")/\1set(CPACK_PACKAGE_VERSION_$1 \"$2\")/" CMakeLists.txt
    rm CMakeLists.txt.bak
}

tag_exists() {
    git show-ref --verify --quiet "refs/tags/$1"
}

# The highest <n> among the tags <base><n>, empty when there is none
latest_number() {
    local pattern
    pattern="^$(sed 's/\./\\./g' <<<"$1")[0-9]+$"
    git tag --list "$1*" | { grep -E "${pattern}" || true; } | sed 's/^.*[^0-9]//' | sort -n | tail -n 1
}

# A tag covers HEAD when it points at HEAD or at a version commit made on top of it
covers_head() {
    git merge-base --is-ancestor HEAD "$1"
}

git_as_actor() {
    git -c user.name="${actor}" -c user.email="${actor}@users.noreply.github.com" "$@"
}

major=$(cmake_version MAJOR)
minor=$(cmake_version MINOR)

release_branch=false
case "${branch}" in
    develop)
        suffix=DB
        ;;
    "releases/v${major}")
        suffix=RC
        release_branch=true
        ;;
    *)
        suffix=${branch//[^a-zA-Z0-9]/_}
        ;;
esac

if [[ "${release}" == true && "${release_branch}" != true ]]; then
    echo "Release tags are created from releases/v${major}, not from ${branch}" >&2
    exit 1
fi

# Once a minor is released, the release branch moves on to the next one
last_release=""
if [[ "${release_branch}" == true ]]; then
    while tag_exists "V${major}.${minor}"; do
        last_release="V${major}.${minor}"
        minor=$((minor + 1))
    done
fi

# The latest tag of the same kind decides whether there is anything new to tag
previous=""
if [[ "${release}" == true ]]; then
    tag="V${major}.${minor}"
    previous=${last_release}
else
    base="V${major}.${minor}${suffix}"
    number=$(latest_number "${base}")
    if [[ -n "${number}" ]]; then
        previous="${base}${number}"
    fi
    number=$((${number:-0} + 1))
    tag="${base}${number}"
fi

if [[ -n "${previous}" ]] && covers_head "${previous}"; then
    echo "Nothing new since ${previous}, no tag created"
    if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
        echo "created=false" >>"${GITHUB_OUTPUT}"
    fi
    exit 0
fi

echo "Next tag for ${branch}: ${tag}"
if [[ "${dry_run}" == true ]]; then
    exit 0
fi

set_cmake_version MINOR "${minor}"
if [[ "${release}" != true ]]; then
    set_cmake_version PRE_RELEASE "${number}"
fi
git add CMakeLists.txt
if ! git diff --cached --quiet; then
    git_as_actor commit --quiet --message "Set version for ${tag}"
fi

git_as_actor tag --annotate --message "Build tag ${tag}" "${tag}"
git push origin "refs/tags/${tag}"
if [[ "${release}" == true ]]; then
    git push origin "HEAD:refs/heads/${branch}"
fi

echo "Created ${tag}"
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    {
        echo "tag=${tag}"
        echo "created=true"
    } >>"${GITHUB_OUTPUT}"
fi
