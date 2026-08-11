#!/bin/sh
# get_demo.sh -- fetch the Nov 1997 Hexen II demo data so Hexenwail has
# something to run.
#
# Hexenwail is a game engine; it ships no game content.  Raven's data is not
# ours to hand out -- the only licence distributed with the demo is
# Activision's retail agreement, which does not grant redistribution (see
# assets/demo/README.md).  So this does not mirror anything: it downloads the
# package from the uHexen2 project, where it has been publicly hosted for two
# decades, under whatever terms apply to it there, and verifies that what
# arrived is what was expected.
#
# The demo is three levels of the Blackmarsh hub.  If you own Hexen II, on
# Steam, GOG or disc, you do not need this -- copy that installation's "data1"
# directory next to the Hexenwail executable instead.
#
# Usage: scripts/get_demo.sh [destination]
#   destination defaults to the current directory; "data1" is created inside
#   it.  Point it at the directory holding the hexenwail/glhexen2 binary --
#   the engine prints that path as "basedir is: ..." on startup.

set -eu

# downloads.sourceforge.net, not the /projects/.../download page: the latter
# answers with a 143 KB "your download will start shortly" HTML interstitial
# unless the client looks like a download tool.  curl and wget both identify as
# themselves and are served the file; anything claiming to be a browser gets
# the page, or a 403 from this host.  Only the checksum below would catch that,
# so leave the URL and the default user agents alone.
URL='https://downloads.sourceforge.net/project/uhexen2/Hexen2Demo-Nov.1997/hexen2demo_nov1997-linux-i586.tgz'
TARBALL='hexen2demo_nov1997-linux-i586.tgz'
SHA256='2df15cde0128b7a036e71995e068ca853f13be8e2b591caac140025d66643fc0'
# Only this subtree is wanted.  The package also carries 1997 i586 Linux
# binaries of the old engine, which are of no use to us.
MEMBER='hexen2demo_nov1997/data1'

dest=${1:-.}

die () {
	echo "get_demo.sh: $*" >&2
	exit 1
}

[ -d "$dest" ] || die "no such directory: $dest"

if [ -e "$dest/data1/pak0.pak" ]; then
	die "$dest/data1/pak0.pak already exists -- refusing to overwrite it.
Remove that data1 directory first if you really want to replace it."
fi

# --- tools -----------------------------------------------------------------

if command -v curl >/dev/null 2>&1; then
	fetch () { curl -fL --progress-bar -o "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
	fetch () { wget -q --show-progress -O "$2" "$1"; }
else
	die 'need curl or wget to download.'
fi

# sha256sum on Linux, shasum on macOS and the BSDs.
if command -v sha256sum >/dev/null 2>&1; then
	sha256_of () { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
	sha256_of () { shasum -a 256 "$1" | cut -d' ' -f1; }
else
	die 'need sha256sum or shasum to verify the download.'
fi

command -v tar >/dev/null 2>&1 || die 'need tar to unpack.'

# --- fetch -----------------------------------------------------------------

work=$(mktemp -d "${TMPDIR:-/tmp}/hexenwail-demo.XXXXXX") || die 'mktemp failed'
trap 'rm -rf "$work"' EXIT INT TERM

echo "Downloading the Hexen II demo (13 MB) from the uHexen2 project..."
fetch "$URL" "$work/$TARBALL" || die 'download failed.'

echo "Verifying..."
got=$(sha256_of "$work/$TARBALL")
if [ "$got" != "$SHA256" ]; then
	die "checksum mismatch -- refusing to install.
  expected $SHA256
  got      $got
The download was corrupted, or the file upstream is not the one this script
was written against."
fi

# --- install ---------------------------------------------------------------

# Unpack beside the destination, then move into place, so an interrupted run
# cannot leave a half-populated data1 that the engine would try to load.
tar -xzf "$work/$TARBALL" -C "$work" --strip-components=1 "$MEMBER" \
	|| die "could not extract $MEMBER from the archive."
[ -f "$work/data1/pak0.pak" ] || die 'archive did not contain data1/pak0.pak.'

mv "$work/data1" "$dest/data1" || die "could not move data1 into $dest"

cat <<EOF

Installed the demo data to $dest/data1

Launch Hexenwail from $dest and it will pick it up; it should print
"Playing the demo version." during startup.  If you run the engine from
somewhere else, point it here with:

    hexenwail -basedir "$(cd "$dest" && pwd)"

This is the three-level demo.  The full game unlocks the rest -- copy the
data1 directory from a Steam, GOG or disc installation over this one.
EOF
