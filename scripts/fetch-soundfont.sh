#!/usr/bin/env bash
# fetch-soundfont.sh OUT.sf2 -- the General MIDI SoundFont the web build ships.
#
# TimGM6mb (Tim Brechbill, GPL-2, ~6 MB): the same SoundFont the Flatpak and
# the linux-fhs release bundle, from the same Debian upstream tarball, pinned
# by the same SHA-256 (flake.nix timgmTar, flatpak/io.github.hexenwail.hexenwail.yml;
# tools/test_soundfont_pin.py keeps the three equal).  deb.debian.org first,
# snapshot.debian.org -- Debian's permanent archive -- if the package has moved
# on.  Nothing is written unless the tarball hashes to the pin.
#
# HEXENWAIL_SOUNDFONT_TARBALL=/path/to/timgm6mb-soundfont_1.3.orig.tar.gz
# uses a local copy instead of the network (it is still checked).  GitHub #114.
set -euo pipefail

out="${1:?usage: fetch-soundfont.sh OUT.sf2}"
sha256="af8f3a00e416dfb262bcaa904a1c84df04a51b72bbc1313aed012bc754bdf99b"
urls=(
	"https://deb.debian.org/debian/pool/main/t/timgm6mb-soundfont/timgm6mb-soundfont_1.3.orig.tar.gz"
	"https://snapshot.debian.org/archive/debian/20240101T000000Z/pool/main/t/timgm6mb-soundfont/timgm6mb-soundfont_1.3.orig.tar.gz"
)

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tarball="$work/timgm6mb.tar.gz"

if [ -n "${HEXENWAIL_SOUNDFONT_TARBALL:-}" ]; then
	cp "$HEXENWAIL_SOUNDFONT_TARBALL" "$tarball"
else
	for url in "${urls[@]}"; do
		if curl -fsSL --retry 3 -o "$tarball" "$url"; then
			break
		fi
		echo "fetch-soundfont: $url failed, trying the next" >&2
		rm -f "$tarball"
	done
	[ -s "$tarball" ] || { echo "fetch-soundfont: no URL served the tarball" >&2; exit 1; }
fi

if command -v sha256sum >/dev/null; then
	got="$(sha256sum "$tarball" | cut -d' ' -f1)"
else
	got="$(shasum -a 256 "$tarball" | cut -d' ' -f1)"
fi
if [ "$got" != "$sha256" ]; then
	echo "fetch-soundfont: tarball sha256 $got is not the pinned $sha256" >&2
	exit 1
fi

tar -xzf "$tarball" -C "$work"
cp "$work/timgm6mb-soundfont_1.3/TimGM6mb.sf2" "$out"
echo "fetch-soundfont: $out ($(wc -c < "$out") bytes, TimGM6mb, tarball sha256 verified)"
