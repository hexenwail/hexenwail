# libsndfile re-audit — 2026-11-10

GitHub #168 (`uhexen2-ql7h`): libsndfile 1.2.2 in the runtime closure, 5 CVEs at
filing (2026-08-11 vulnix audit). This is the re-audit the issue's
done-condition calls for: watch for a libsndfile bump in nixpkgs, re-run the
audit, then act. The bump exists — in part. The pin has not moved yet.

Recorded pinned libsndfile version: 1.2.2

## 1. The pin

- `flake.lock` pins nixpkgs to rev `2fcb964de67fcf60b43471c55d5d99e61a9ccb5a`,
  `lastModified` 2026-08-10 17:52 UTC (`nixos-unstable` ref). The lock was last
  touched 2026-09-22 (#248) but the nixpkgs rev inside is unchanged since the
  issue was filed — the pin is ~3 months stale as of this writing.
- libsndfile at that rev is **1.2.2** with exactly one patch (gcc15 build fix,
  libsndfile PR #1055, upstream commit `2251737b`); no CVE backports.
- `scripts/check-libsndfile-pin.sh` re-checks this mechanically (pure
  curl + grep, no nix): it fails loudly if the pinned version ever differs
  from the version recorded above, so a pin move without a re-audit is
  visible instead of silent.

## 2. Per-CVE status: pinned rev vs `nixos-unstable` HEAD

Method: fetched `pkgs/by-name/li/libsndfile/package.nix` at the pinned rev
and at `nixos-unstable` HEAD (2026-11-10) from raw.githubusercontent.com and
diffed them; CVE status cross-checked against the Debian security tracker,
Ubuntu CVE tracker, SUSE CVE pages, openSUSE changelogs and an OpenEmbedded
cve-check report. Current `nixos-unstable` HEAD still ships libsndfile
**1.2.2** — no upstream release since 1.2.2 (2023-08-13) — but its
`package.nix` gained CVE backports on 2026-10-17 (nixpkgs commit
`91d5f8b1e134`, "libsndfile: add patches for CVE-2024-50612, CVE-2026-37555";
also PR #575364, merged 2026-10-21 as `14a6896509d7`).

| CVE | CVSS | Where | Pinned rev (2026-08-10) | `nixos-unstable` HEAD (2026-11-10) |
| --- | --- | --- | --- | --- |
| CVE-2024-50612 | 5.5 | OOB read, `vorbis_analysis_wrote`, `ogg_vorbis.c` | unpatched | **patched** — fetchpatch of upstream commit `4755f5b` (libsndfile PR #1045) |
| CVE-2026-37555 | 7.5 | int overflow, IMA ADPCM codec | unpatched | **patched** — two fetchpatches, upstream commits `9a82911` (AIFF path, PR #979) + `cdadf9a` (WAV/close paths, PR #1123) |
| CVE-2025-52194 | 7.5 | buffer overflow, `ircam_read_header`, `src/ircam.c:164` | unpatched | unpatched — see §3 |
| CVE-2024-50613 | 6.5 | reachable assertion, `mpegl3encoder_close`, `mpegl3encode.c` | unpatched | unpatched |
| CVE-2025-56226 | 5.3 | memory leak, `mpeg_l3_encoder_init`, `mpegl3encode.c` | unpatched | unpatched in nixpkgs (openSUSE patched it 2026-01-15; Debian fixed in trixie/forky/sid) |

A `nix flake update` today would clear **2 of 5**, not 5 of 5.

## 3. Reachability assessment

- **CVE-2024-50612 matters most to this project specifically.** It is in
  `ogg_vorbis.c` — the exact parser the SF3 path exercises (SF3 samples are
  Ogg Vorbis). It is also the one nixpkgs has now backported, so the bump
  clears the highest-relevance CVE.
- **CVE-2026-37555** is an IMA ADPCM *decode* bug, reachable in principle
  through `sf_open_virtual` format autodetection on a crafted soundfont.
  Also cleared by the bump.
- **CVE-2025-52194 is the remaining genuinely reachable one**: a decode-path
  parser (`ircam_read_header`), reachable in principle the same way. Its
  status has changed since the last audit, in a good direction: an upstream
  fix now exists **on master** — commit `7ff854d1` ("ircam: use `psf_lrintf`
  when reading samplerate", #1147, 2026-07-21, `src/ircam.c` only). The
  earlier fix candidate, PR #1099 ("ircam: harden header parsing"), was
  closed 2026-10-27 as superseded by it. No release contains the fix yet
  (latest release is still 1.2.2), and nixpkgs has not backported it, so the
  bump alone does not clear this CVE. Candidate backport source if the
  maintainer wants it before upstream releases: upstream commit `7ff854d1`
  (preferred — it is the merged fix; the openSUSE patch
  `libsndfile-CVE-2025-52194.patch`, May 2026, is the alternative).
  Caveat, recorded honestly: the reproducer behind the CVE showed a
  float-to-int cast overflow (UBSan) in the samplerate path, while ASan-only
  showed no OOB/corruption with the #1082 PoC; the CVE text claims memory
  corruption. The merged fix addresses the cast. Whether that fully answers
  the CVE as described is a maintainer call, not something this audit can
  settle from the PoC alone.
- **CVE-2024-50613 and CVE-2025-56226 are both in `mpegl3encode.c`** — the
  MP3 *encoder* (`mpeg_l3_encoder_init` / `mpegl3encoder_close`).
  FluidSynth only *decodes* soundfont samples; the engine never encodes MP3.
  Both are credible "not affecting the soundfont path" candidates — but that
  assessment needs a **maintainer ruling recorded here**, not an autonomous
  assertion, because libsndfile's encoder is linked into the same `.so` and
  the claim rests on "FluidSynth never calls the encode API". Until that
  ruling is recorded, both stay open in the table above.

## 4. Exposure inventory, corrected

Verified against the tree at `346edd553` (this audit re-checked every row
rather than carrying the filing-time claims forward).

| Distribution | libsndfile present? | Notes |
| --- | --- | --- |
| Linux zip / AppImage (`packages.linux-fhs`, `appimage`) | **No** — removed by #428 | `fluidsynthPortable` (`flake.nix:863-880`) builds fluidsynth with `-Denable-libsndfile=off` + `patches/fluidsynth-sf3-vorbisfile.patch` (SF3 decoded by libvorbisfile, bit-identical output on 124 samples); the build fails if `libsndfile.so*` reappears (`flake.nix:953-960` guard) |
| NixOS package (`packages.nixos`, `flake.nix:288`) | **Yes** — stock nixpkgs fluidsynth | `buildInputs` at `flake.nix:243-255` lists `fluidsynth`, `libsndfile`, `flac` (transitive pkg-config deps); links `libfluidsynth.so.3` → `libsndfile.so.1` |
| Dev shell (`devShells.default`, `flake.nix:1805+`) | **Yes** | Same stock `fluidsynth` / `libsndfile` / `flac` in buildInputs (`flake.nix:1812-1813`) |
| Flatpak (`flatpak/io.github.hexenwail.hexenwail.yml:65`) | **No, but SF3 is broken there** | FluidSynth module has `-Denable-libsndfile=off` but does **not** apply `patches/fluidsynth-sf3-vorbisfile.patch`, so SF3 soundfonts cannot decode in the Flatpak (bundled default is SF2 TimGM6mb/FluidR3, so default MIDI is unaffected; a user-supplied SF3 would fail). The Oct 17 comment already notes the patch "would apply… if wanted" |
| Windows (`packages.windows`, `flake.nix:966-1138`) | **No — correction to the Oct 17 comment** | The comment says Windows is "still on libsndfile (distribution fluidsynth)", but the current tree shows no fluidsynth anywhere on Windows: MIDI is the native midiStream driver (`engine/CMakeLists.txt:779`, `midi_win.c`), `oslibs/windows/codecs/` contains only ogg/vorbis, and the Windows installPhase bundles SDL3/ogg/vorbis/opusfile/libxmp DLLs only. No fluidsynth DLL, no libsndfile, and no `fluidsynth`/`libsndfile` hits anywhere in `flake.nix:966-1500`. Treat as "no evidence of exposure in the current tree" — worth one confirming line from the maintainer (or an `ldd`-style check of the shipped Windows binaries) rather than a code change |

Untrusted-input path confirmed still live for the NixOS/dev-shell builds:
`engine/h2shared/soundfont.c:31` (`snd_soundfont`, `CVAR_ARCHIVE`) and
`engine/h2shared/midi_fluid.c:97-124` (cvar callback reloads the font
mid-session); `soundfont.c` sniffs the container rather than the extension.

The vulnix whitelist (`.github/vulnix-whitelist.toml`) already records
libsndfile as deliberately NOT whitelisted ("reachable… Tracked separately
in uhexen2-ql7h"), so the weekly scan still flags it. The weekly
`security-scan.yml` ran successfully as recently as 2026-11-09 (and
2026-11-02, 2026-10-26, 2026-10-19) and fires on `flake.lock` PRs, so a bump
PR gets a fresh scan automatically.

## 5. Next step

Bump the nixpkgs pin (maintainer runs `nix flake update nixpkgs`; the weekly
scan re-runs on the lock PR). That clears 2 of 5. The issue stays open until:

1. CVE-2025-52194 is addressed — upstream merge is done (`7ff854d1`), so
   this now means a nixpkgs backport of that commit, a project-side patch,
   or a recorded ruling; and
2. the encoder-only ruling for CVE-2024-50613 / CVE-2025-56226 is recorded
   (§3); and
3. the Windows non-exposure correction is confirmed by the maintainer (§4).

Out of scope for this audit patch, flagged for the maintainer: applying
`fluidsynth-sf3-vorbisfile.patch` to the Flatpak module (restores SF3 in the
Flatpak; unverifiable here, no flatpak build), and backporting `7ff854d1`
into the project's fluidsynth override (changes shipped code paths — needs
the maintainer's call; the issue carries the `decision` label for exactly
this).

## 6. Method note

Every version fact above comes from fetching the pinned files over HTTPS from
GitHub and diffing them locally (pinned rev `package.nix` vs
`nixos-unstable` HEAD `package.nix`; libsndfile releases API; PR #1099
comments/timeline; nixpkgs commit search). No nix was available in the
container where this audit was written (nix not on PATH after a VM wipe), so
no `nix flake update`, nix eval, or nix build ran here — and none is needed
for this slice, which is a doc plus a curl/grep check script. The bump
itself is a maintainer/CI action; its acceptance check is the weekly scan
result on the lock PR (expect libsndfile findings to drop from 5 CVEs to 3:
52194, 50613, 56226 — with 50612/37555 gone).
