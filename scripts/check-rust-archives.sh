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
common=hashindex,mathlib,sizebuf,crc,link_ops,msg_io,strlcpy,strlcat,cvar,zone,cmd,quakefs,world_trace,world_hull,world_link,world_move,sv_move,sv_phys,sv_phys_push,sv_phys_walk,sv_phys_move

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
        glhexen2) variant=client; features=$common,wad,net_loop_h2,net_bsd_h2,net_udp_h2,huffman,net_udp_hw,net_chan,sv_main_sound,sv_main_ents,sv_main_datagram,sv_main_connect;
            wad=1; info=0; hw=1 ;;
        h2ded) variant=h2ded; features=$common,net_bsd_h2,net_udp_h2,h2ded_target,sv_main_sound,sv_main_ents,sv_main_datagram,sv_main_connect;
            wad=0; info=0; hw=0 ;;
        hwsv) variant=hwsv; features=$common,info_str,huffman,net_udp_hw,net_chan,world_trace_hw;
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
        check_count "$file" SV_RecursiveHullCheck 1
        check_count "$file" SV_HullPointContents 1
        check_count "$file" SV_HullForEntity 1
        check_count "$file" SV_InitBoxHull 1
        check_count "$file" SV_LinkEdict 1
        check_count "$file" SV_UnlinkEdict 1
        check_count "$file" SV_ClearWorld 1
        check_count "$file" SV_Move 1
        check_count "$file" SV_PointContents 1
        check_count "$file" SV_TestEntityPosition 1
        # Hexen II's sv_main.c ports (#469): the client's listen server and
        # h2ded only.  hwsv has no SV_MaxSounds at all
        check_count "$file" SV_MaxSounds "$h2"
        check_count "$file" SV_PrepareClientEntities "$h2"
    done
    # hwsv has SV_StartParticle4 and the other message functions too, but as
    # HexenWorld's own C (sv_send.c): its archive must not carry the Rust
    check_count "$archive" SV_StartParticle4 "$h2"
    check_count "$binary" SV_StartParticle4 1
    # the datagram port (#469 slice C) is the same shape: hwsv defines its own
    # SV_SendClientMessages, the other two get it from the archive
    check_count "$archive" SV_SendClientMessages "$h2"
    check_count "$binary" SV_SendClientMessages 1
    # the connect/signon port (#469 slice D): the archive carries these in the
    # Hexen II targets only.  hwsv's binary has none of them except
    # SV_ModelIndex, which is HexenWorld's own C in sv_init.c; the Hexen II
    # binaries get exactly one of each from the archive.
    for sym in SV_CheckForNewClients SV_ModelIndex SV_ReserveSignonSpace SV_SendSignonBuffer SV_SendServerinfo; do
        check_count "$archive" "$sym" "$h2"
        case "$sym" in SV_ModelIndex) check_count "$binary" "$sym" 1 ;; *) check_count "$binary" "$sym" "$h2" ;; esac
    done
    # The hull traces' fatal path names the target's own error function: the
    # hwsv archive must reach SV_Error (world_trace_hw) and the other two must
    # not, since only hwsv defines it -- an archive that did would fail to
    # link there, or silently report through the wrong path (#455).
    sv_error_refs=$(nm -u "$archive" 2>/dev/null | grep -cE ' U SV_Error$' || true)
    if [ "$target" = hwsv ]; then want_sv_error=1; else want_sv_error=0; fi
    if [ "$(( sv_error_refs > 0 ))" -ne "$want_sv_error" ]; then
        echo "FAIL: $variant archive references SV_Error $sv_error_refs time(s); expected $([ "$want_sv_error" = 1 ] && echo 'at least once' || echo none)" >&2
        exit 1
    fi
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
