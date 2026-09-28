#!/usr/bin/env bash
#
# package-release.sh -- lay out the Linux and Windows bundles from .#release.
#
# Usage: package-release.sh [--flat-linux] [--zip VERSION] RELEASE_DIR OUT_DIR
#
#   RELEASE_DIR  the nix output's release tree, normally result/release
#   OUT_DIR      created; receives linux/ and windows/, the two staged trees
#   --zip VERSION
#                also write OUT_DIR/hexenwail-VERSION-linux-x86_64.zip and
#                OUT_DIR/hexenwail-VERSION-windows-x86_64.zip (release.yml)
#   --flat-linux lay linux-x86_64/'s contents out at the root of linux/
#                instead of keeping the directory (tester-build.yml)
#
# release.yml and tester-build.yml used to carry one inline copy of this each.
# The copies had to be edited in lockstep for the glh2 -> hexenwail rename,
# and a tester running a differently laid-out build than users get defeats
# the tester build's purpose, so both call this now.
#
# The Linux tree is linux-x86_64/ whole, never bin/glhexen2 alone: that cannot
# start, because linux-fhs patchelfs the binary to rpath $ORIGIN/../lib and
# bundles 36 shared objects there (uhexen2-3aet), and bin/ also carries
# soundfont.sf2.  gamecode/ is a shared staging tree, not part of any platform
# dir, so it has to be named here or it ships nowhere (uhexen2-8qp3).  The
# engine loads Linux gamecode from linux-x86_64/share/hexenwail/; the root
# gamecode/ is the spare copy BUILD_INFO.txt and gamecode/README.txt describe.
#
# The Windows tree is Hexen II's flat layout -- hexenwail.exe and its DLLs at
# the root beside an empty data1/ -- so users extract straight into their
# Hexen II directory and drop pak0.pak/pak1.pak into data1/.
#
# --flat-linux exists only to keep the tester build's Linux layout what it has
# always been; the release zip keeps linux-x86_64/ as a directory.  They
# differ today, and unifying them is a separate decision.
#
# Requires cp, zip (with --zip) -- present on the GitHub runner.
#
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

flat_linux=0
version=''
args=()
while [ "$#" -gt 0 ]; do
	case "$1" in
		-h|--help) sed -n '2,/^# SPDX/p' "$0"; exit 0 ;;
		--flat-linux) flat_linux=1 ;;
		--zip)
			# Empty would silently skip the zips; a leading dash is the next
			# flag swallowed as the version (--zip --flat-linux A B).
			case "${2-}" in
				''|-*) echo "error: --zip needs a VERSION, got '${2-}'" >&2; exit 2 ;;
			esac
			version=$2; shift ;;
		-*) echo "unknown argument: $1 (try --help)" >&2; exit 2 ;;
		*) args+=("$1") ;;
	esac
	shift
done
[ "${#args[@]}" -eq 2 ] || { echo "usage: $0 [--flat-linux] [--zip VERSION] RELEASE_DIR OUT_DIR" >&2; exit 2; }
src=${args[0]}
out=${args[1]}
[ -d "$src" ] || { echo "error: $src is not a directory" >&2; exit 2; }
# Never deleted for you: the copies are read-only, as the store is.
[ ! -e "$out" ] || {
	echo "error: $out already exists; remove it with: chmod -R u+w '$out' && rm -rf '$out'" >&2
	exit 2
}

linux="$out/linux"
win="$out/windows"
mkdir -p "$linux" "$win/data1"

# -L throughout: the nix output is symlinks into the store, and a bundle of
# symlinks is empty everywhere but the machine that built it.
if [ "$flat_linux" -eq 1 ]; then
	cp -rL "$src/linux-x86_64/." "$linux/"
else
	cp -rL "$src/linux-x86_64" "$linux/"
fi
cp -rL "$src/gamecode" "$src/licenses" "$linux/"
cp -L "$src/BUILD_INFO.txt" "$linux/"

# bin/* is what makes it runnable without a toolchain: the win64 derivation
# installs SDL3.dll and the codec DLLs into bin/ beside hexenwail.exe.
cp -L "$src"/windows-x86_64/bin/* "$win/"
test -f "$win/hexenwail.exe"
test ! -e "$win/glh2.exe"
# get_demo.* live beside bin/, not in it, so copying bin/* alone shipped a
# bundle whose own BUILD_INFO.txt told users to run a file that was not there.
cp -L "$src/windows-x86_64/get_demo.ps1" "$src/windows-x86_64/get_demo.cmd" "$win/"
# gamecode/ stays a subdirectory even in the flat layout.  Windows has no
# share/hexenwail layer: gamecode/ beside hexenwail.exe is the copy
# PR_FindBundleDir() loads, so without it the player runs their install's
# retail progs.dat instead of ours (uhexen2-biyi).  And putting progs.dat at
# data1/progs.dat instead would overwrite the player's retail gamecode on
# unzip, with no copy in any .pak to restore from.  Keep the copy deliberate.
cp -rL "$src/gamecode" "$win/"
test -f "$win/gamecode/data1/progs.dat"
cp -rL "$src/licenses" "$win/"
cp -L "$src/BUILD_INFO.txt" "$win/"
echo "Place your Hexen II pak files (pak0.pak, pak1.pak) in this folder." \
	>"$win/data1/README - put pak files here.txt"

if [ -n "$version" ]; then
	# Paths inside the zips are relative to each tree's root, so the entries
	# are exactly what users have always unpacked.
	linux_zip="hexenwail-$version-linux-x86_64.zip"
	win_zip="hexenwail-$version-windows-x86_64.zip"
	if [ "$flat_linux" -eq 1 ]; then
		(cd "$linux" && zip -qr "../$linux_zip" .)
	else
		(cd "$linux" && zip -qr "../$linux_zip" \
			linux-x86_64/ gamecode/ licenses/ BUILD_INFO.txt)
	fi
	(cd "$win" && zip -qr "../$win_zip" .)
	echo "wrote $out/$linux_zip and $out/$win_zip"
fi
