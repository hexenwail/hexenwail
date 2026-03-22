#!/usr/bin/env bash
# Usage: ./release.sh v0.6.5-alpha
#
# Tags HEAD and pushes to GitHub, triggering the Actions release workflow
# which builds Linux + Windows binaries and creates a GitHub prerelease.
#
# Prerequisites: gh (authenticated), git remote "bobberb"

set -euo pipefail

VERSION="${1:?Usage: ./release.sh <version-tag>}"
REMOTE="bobberb"

echo "=== Releasing $VERSION ==="

# Ensure clean tree
if [[ -n "$(git status --porcelain)" ]]; then
    echo "ERROR: working tree is dirty, commit first"
    exit 1
fi

# Find previous tag
PREV_TAG=$(git describe --tags --abbrev=0 HEAD 2>/dev/null || echo "")
if [[ -z "$PREV_TAG" ]]; then
    echo "WARNING: no previous tag found"
    RANGE="HEAD"
else
    echo "Previous release: $PREV_TAG"
    RANGE="${PREV_TAG}..HEAD"
fi

# Show what will be released
echo ""
echo "--- Commits ---"
git log "$RANGE" --no-merges --format="- %s (%h)" | grep -v "^- Sync beads" || true
echo "--- End ---"
echo ""
read -rp "Tag and push $VERSION? GitHub Actions will build the release. [y/N] " confirm
if [[ "$confirm" != [yY] ]]; then
    echo "Aborted."
    exit 0
fi

# Tag and push — Actions release workflow triggers on v* tags
git tag "$VERSION" HEAD
if ! git push "$REMOTE" master "$VERSION"; then
    echo "ERROR: push failed, deleting local tag"
    git tag -d "$VERSION"
    exit 1
fi

echo ""
echo "=== Tagged $VERSION and pushed to $REMOTE ==="
echo "GitHub Actions will build and create the release."
echo "Watch progress: gh run watch"
echo "Release URL: https://github.com/bobberb/hexenwail/releases/tag/$VERSION"
