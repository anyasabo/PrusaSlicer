#!/usr/bin/env bash
# Uploads the files of build/package to the GitHub release of a tag. If the release does not exist, it is made, as a
# pre-release (the base is an upstream alpha). A file with the same name is replaced.
# The CI workflow and a local machine (for example the Linux build of a devspace) run this same script.
#
# Usage: .github/fork/upload-release.sh TAG
# Environment: GH_TOKEN (in CI), GH_REPO (default: anyasabo/PrusaSlicer).
set -euo pipefail

cd "$(dirname "$0")/../.."
tag=${1:?usage: $0 TAG}
export GH_REPO=${GH_REPO:-anyasabo/PrusaSlicer}
case "$tag" in v*-anya.*) ;; *) echo "not a fork release tag: $tag (v<version>-anya.<n>)" >&2; exit 1 ;; esac
git rev-parse -q --verify "refs/tags/$tag" >/dev/null || { echo "no local tag $tag" >&2; exit 1; }

files=(build/package/*.dmg build/package/*.tar.gz build/package/*.AppImage build/package/SHA256SUMS-*.txt)
existing=()
for f in "${files[@]}"; do
    [ -f "$f" ] && existing+=("$f")
done
[ ${#existing[@]} -gt 0 ] || { echo "nothing in build/package" >&2; exit 1; }

if ! gh release view "$tag" >/dev/null 2>&1; then
    # Two jobs can make the release at the same time. If the create fails, the release must exist now.
    gh release create "$tag" --verify-tag --prerelease \
        --title "PrusaSlicer ${tag#v} (fork anyasabo/PrusaSlicer)" \
        --notes-file .github/fork/RELEASE_NOTES.md || gh release view "$tag" >/dev/null
fi
gh release upload "$tag" "${existing[@]}" --clobber
gh release view "$tag" --json assets --jq '.assets[].name'
