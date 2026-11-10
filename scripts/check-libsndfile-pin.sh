#!/bin/sh
# check-libsndfile-pin.sh — re-check the libsndfile CVE picture for GitHub #168
# (uhexen2-ql7h) without nix.
#
# Reads the pinned nixpkgs rev from flake.lock, fetches
# pkgs/by-name/li/libsndfile/package.nix at that rev and at nixos-unstable
# HEAD from raw.githubusercontent.com, and reports the libsndfile version at
# each rev plus, for each of the 5 CVEs from the 2026-08-11 vulnix audit,
# whether a corresponding patch marker (CVE id or upstream commit hash) is
# present in that package.nix.
#
# Exit status:
#   0  pinned version matches the version recorded in the audit doc
#      (docs/security/libsndfile-audit-2026-11.md) — no re-audit needed
#   1  pinned version differs from the recorded version: the pin moved
#      without a re-audit — update the audit doc, then this script
#   2  usage error, fetch failure, or the recorded version cannot be read
#
# Dependencies: curl, grep, sed, mktemp. No nix, no vulnix, no NVD feed.
# Run from the repo root, or anywhere (it finds the root from its own path).
#
# The marker table (upstream commit hashes from the audit doc):
#   CVE-2024-50612  4755f5b  (nixpkgs backport, 2026-10-17)
#   CVE-2026-37555  9a82911 cdadf9a (nixpkgs backport, 2026-10-17)
#   CVE-2025-52194  7ff854d  (merged upstream fix, #1147; no nixpkgs backport yet)
#   CVE-2024-50613  (no known upstream fix; marker is the CVE id itself)
#   CVE-2025-56226  (no known upstream fix; marker is the CVE id itself)

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOCK="$ROOT/flake.lock"
DOC="$ROOT/docs/security/libsndfile-audit-2026-11.md"
PKG_PATH="pkgs/by-name/li/libsndfile/package.nix"

die() { echo "check-libsndfile-pin: $*" >&2; exit 2; }

[ -f "$LOCK" ] || die "flake.lock not found at $LOCK"
[ -f "$DOC" ] || die "audit doc not found at $DOC (run from the repo root)"

# Version recorded in the audit doc, e.g. "Recorded pinned libsndfile version: 1.2.2"
RECORDED="$(sed -n 's/^Recorded pinned libsndfile version: \([0-9][0-9.]*\)$/\1/p' "$DOC" | head -1)"
[ -n "$RECORDED" ] || die "no 'Recorded pinned libsndfile version:' line in $DOC"

# Pinned nixpkgs rev from flake.lock (nodes.nixpkgs.locked.rev).
START="$(grep -n '"nixpkgs"' "$LOCK" | head -1 | cut -d: -f1)"
[ -n "$START" ] || die "no nixpkgs node in $LOCK"
REV="$(sed -n "${START},+14p" "$LOCK" | grep -m1 '"rev"' | sed 's/.*"rev": *"\([0-9a-f]*\)".*/\1/')"
[ -n "$REV" ] || die "could not read the pinned nixpkgs rev from $LOCK"
echo "pinned nixpkgs rev: $REV"

TMP="$(mktemp -d)" || die "mktemp failed"
trap 'rm -rf "$TMP"' EXIT INT TERM

fetch() { # $1 = rev-or-branch, $2 = out file
    curl -fsSL "https://raw.githubusercontent.com/NixOS/nixpkgs/$1/$PKG_PATH" -o "$2" \
        || die "failed to fetch $PKG_PATH at $1 (network or rev gone?)"
}

version_of() { # $1 = package.nix file
    grep -m1 'version = "' "$1" | sed 's/.*version = "\([^"]*\)".*/\1/'
}

# CVE -> marker patterns (CVE id or upstream commit hash), space-separated.
cve_markers() {
    case "$1" in
        CVE-2024-50612) echo "CVE-2024-50612 4755f5b" ;;
        CVE-2026-37555) echo "CVE-2026-37555 9a82911 cdadf9a" ;;
        CVE-2025-52194) echo "CVE-2025-52194 7ff854d" ;;
        CVE-2024-50613) echo "CVE-2024-50613" ;;
        CVE-2025-56226) echo "CVE-2025-56226" ;;
    esac
}

patched() { # $1 = package.nix file, $2 = CVE id; prints yes/no
    for m in $(cve_markers "$2"); do
        if grep -q "$m" "$1"; then echo yes; return; fi
    done
    echo no
}

fetch "$REV" "$TMP/pinned.nix"
fetch "nixos-unstable" "$TMP/head.nix"

PINNED_VER="$(version_of "$TMP/pinned.nix")"
HEAD_VER="$(version_of "$TMP/head.nix")"
[ -n "$PINNED_VER" ] || die "no version line in pinned package.nix"
[ -n "$HEAD_VER" ] || die "no version line in HEAD package.nix"

echo "libsndfile version at pinned rev: $PINNED_VER"
echo "libsndfile version at nixos-unstable HEAD: $HEAD_VER"
echo
printf '%-16s %-8s %-8s\n' "CVE" "pinned" "HEAD"
for cve in CVE-2024-50612 CVE-2026-37555 CVE-2025-52194 CVE-2024-50613 CVE-2025-56226; do
    printf '%-16s %-8s %-8s\n' "$cve" "$(patched "$TMP/pinned.nix" "$cve")" "$(patched "$TMP/head.nix" "$cve")"
done
echo

if [ "$PINNED_VER" != "$RECORDED" ]; then
    echo "FAIL: pinned libsndfile is $PINNED_VER but the audit doc records $RECORDED." >&2
    echo "The pin moved without a re-audit — update docs/security/libsndfile-audit-2026-11.md, then this script." >&2
    exit 1
fi
echo "OK: pinned libsndfile $PINNED_VER matches the audit doc ($RECORDED)."
