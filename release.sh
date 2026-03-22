#!/usr/bin/env bash
# Usage: ./release.sh v0.6.5-alpha
#
# Tags HEAD, builds Linux + Windows binaries with Nix,
# creates a GitHub prerelease with zips attached.
# Auto-generates changelog from commits since last tag.
#
# Prerequisites: nix, gh (authenticated), git remote "bobberb"

set -euo pipefail

VERSION="${1:?Usage: ./release.sh <version-tag>}"
REPO="bobberb/hexenwail"
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
    echo "WARNING: no previous tag found, changelog will include all commits"
    RANGE="HEAD"
else
    echo "Previous release: $PREV_TAG"
    RANGE="${PREV_TAG}..HEAD"
fi

# Generate changelog
CHANGELOG=$(cat <<HEADER
## What's changed since $PREV_TAG

### Commits

HEADER
)
CHANGELOG+=$(git log "$RANGE" --no-merges --format="- %s (\`%h\`)" | grep -v "^- Sync beads")
CHANGELOG+=$(cat <<FOOTER


## Downloads

- **Linux x86_64** — extract and run \`glhexen2\` from your Hexen II data directory
- **Windows x86_64** — extract all files to your Hexen II directory, run \`glh2.exe\`

Requires game data files (\`data1/pak0.pak\`, \`data1/pak1.pak\`).

**Requirements:** OpenGL 4.3 capable GPU (2012+)
FOOTER
)

echo ""
echo "--- Release notes ---"
echo "$CHANGELOG"
echo "--- End ---"
echo ""
read -rp "Proceed? [y/N] " confirm
if [[ "$confirm" != [yY] ]]; then
    echo "Aborted."
    exit 0
fi

# Tag and push
git tag "$VERSION" HEAD
git push "$REMOTE" master "$VERSION"

# Build
echo "Building release binaries..."
nix build .#release --print-build-logs

# Package
PKGDIR=$(mktemp -d)
cd result/release
zip -r "$PKGDIR/hexenwail-${VERSION}-linux-x86_64.zip" linux-x86_64/ licenses/ BUILD_INFO.txt
zip -r "$PKGDIR/hexenwail-${VERSION}-windows-x86_64.zip" windows-x86_64/ licenses/ BUILD_INFO.txt

# Create release
echo ""
echo "Creating GitHub release..."
gh release create "$VERSION" -R "$REPO" --prerelease --title "$VERSION" \
    --notes "$CHANGELOG" \
    "$PKGDIR/hexenwail-${VERSION}-linux-x86_64.zip" \
    "$PKGDIR/hexenwail-${VERSION}-windows-x86_64.zip"

rm -rf "$PKGDIR"

echo ""
echo "=== Done: https://github.com/$REPO/releases/tag/$VERSION ==="
