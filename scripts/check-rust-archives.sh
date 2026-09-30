#!/usr/bin/env bash
# check-rust-archives.sh -- #233: one target-specific Rust staticlib per binary.
# Requires a native three-target build (shared by check-rust.sh). Run in nix develop.
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/lib/rust-gate.sh
. "$root/scripts/lib/rust-gate.sh"
work="${WORKDIR:-$(mktemp -d)}"
mkdir -p "$work"
for tool in cmake nm; do
    command -v "$tool" >/dev/null || { echo "FAIL: $tool missing" >&2; exit 2; }
done
build=$(rust_gate_engine_build "$work")
common=hashindex,mathlib,sizebuf,crc,link_ops,msg_io,strlcpy,strlcat,cvar,zone,cmd,quakefs

check_count() { # file symbol expected-count
    local count
    count=$(nm -g --defined-only "$1" | grep -cE " [A-Z] $2\$" || true)
    if [ "$count" -ne "$3" ]; then
        echo "FAIL: $1 has $count definitions of $2; expected $3" >&2
        exit 1
    fi
}

for target in glhexen2 h2ded hwsv; do
    case "$target" in
        glhexen2) variant=client; features=$common,wad,net_loop_h2,net_bsd_h2,net_udp_h2,huffman,net_udp_hw,net_chan;
            wad=1; info=0; hw=1 ;;
        h2ded) variant=h2ded; features=$common,net_bsd_h2,net_udp_h2,h2ded_target;
            wad=0; info=0; hw=0 ;;
        hwsv) variant=hwsv; features=$common,info_str,huffman,net_udp_hw,net_chan;
            wad=0; info=1; hw=1 ;;
    esac
    archive="$build/rust/$variant/libengine_rs.a"
    manifest="$build/rust/archive-$target.txt"
    links="$build/rust/link-libraries-$target.txt"
    binary="$build/bin/$target"
    expected="$target|$archive|$features"
    [ "$(cat "$manifest")" = "$expected" ] || {
        echo "FAIL: archive/feature mapping in $manifest differs from $expected" >&2
        exit 1
    }
    [ -f "$archive" ] && [ -x "$binary" ] || {
        echo "FAIL: missing archive or binary for $target" >&2; exit 1;
    }
    # The generated LINK_LIBRARIES property works for both Make and Ninja.
    # Check the concrete Make link command too, when available.
    [ -f "$links" ] || { echo "FAIL: missing link manifest $links" >&2; exit 1; }
    for link in "$links" "$build/CMakeFiles/$target.dir/link.txt"; do
        [ -f "$link" ] || continue
        found=$(grep -oE 'rust/(client|h2ded|hwsv)/(libengine_rs\.a|engine_rs\.lib)' "$link" || true)
        [ "$found" = "rust/$variant/libengine_rs.a" ] || {
            echo "FAIL: $target links '$found', expected exactly rust/$variant/libengine_rs.a ($link)" >&2
            exit 1
        }
        if grep -Eq '(-z,muldefs|--allow-multiple-definition)' "$link"; then
            echo "FAIL: $target uses duplicate-definition linker workaround" >&2
            exit 1
        fi
    done
    # Link and archive must agree on ownership: target-only ports are absent,
    # not merely unused, in the other two variants. Each binary has exactly
    # one Rust panic/personality definition from its one linked archive.
    for file in "$archive" "$binary"; do
        check_count "$file" rust_eh_personality 1
        check_count "$file" W_LoadWadFile "$wad"
        check_count "$file" Info_ValueForKey "$info"
        check_count "$file" HWNET_Init "$hw"
        if [ "$target" = hwsv ]; then h2=0; else h2=1; fi
        check_count "$file" net_landrivers "$h2"
        check_count "$file" UDP_Init "$h2"
        if [ "$target" = glhexen2 ]; then loop=1; else loop=0; fi
        check_count "$file" Loop_Init "$loop"
    done
    echo "  $target: exactly one $variant archive; correct features and symbol ownership"
done

# Three physically distinct output and Cargo target directories are required
# even when the same native build runs the Cargo recipes concurrently.
for variant in client h2ded hwsv; do
    [ -d "$build/rust/target-$variant/release" ] || {
        echo "FAIL: no isolated Cargo output for $variant" >&2; exit 1;
    }
done
echo "PASS: one target-specific Rust staticlib per binary; no duplicate-runtime workaround"
