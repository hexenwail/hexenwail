{
  description = "Hexenwail - modernized Hexen II engine (fork of Hammer of Thyrion / uHexen2)";

  nixConfig = {
    extra-substituters = [ "https://hexenwail.cachix.org" ];
    extra-trusted-public-keys = [ "hexenwail.cachix.org-1:8p4Jk7hUQz7PC4eqiqBl0RtorLGO9QosIaKfRa2EgPE=" ];
  };

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachSystem [ "x86_64-linux" ] (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          # packages.demodata is marked unfree on purpose (uhexen2-3vmk: the
          # Nov 1997 demo carries Activision's retail EULA and no
          # redistribution grant).  Permit that one derivation by name so a
          # user who wants it gets a working `nix build .#demodata` instead of
          # having to pass --impure; everything else stays refused as before.
          config.allowUnfreePredicate = pkg:
            nixpkgs.lib.getName pkg == "hexen2-demodata";
        };
        pkgsCross64 = import nixpkgs {
          inherit system;
          crossSystem = {
            config = "x86_64-w64-mingw32";
          };
        };

        # Version: extracted from engine/hexen2/quakedef.h HW_BASE_VERSION
        version = let
          lines = builtins.split "\n" (builtins.readFile ./engine/hexen2/quakedef.h);
          matches = builtins.filter (x: builtins.isString x &&
            builtins.match ".*HW_BASE_VERSION.*" x != null) lines;
          line = builtins.head matches;
          parts = builtins.split "\"" line;
          strs = builtins.filter builtins.isString parts;
        in builtins.elemAt strs 1;

        # Source filter: what the CMake builds actually read, and nothing else.
        #
        # An allowlist rather than a denylist, because the failure modes are
        # not symmetric: forgetting to deny something silently costs cache
        # hits forever, while forgetting to allow something fails the build
        # immediately and loudly.  Note this runs *after* git has already
        # dropped .gitignored paths (result, capture.rdc, the demo tarball,
        # build artefacts), so it only has to reason about tracked files.
        #
        # Why each entry is here -- check before removing one:
        #   CMakeLists.txt  the root list; `utils` configures against it
        #                   (BUILD_UTILS=ON), the engine packages `cd engine`
        #   engine          the client, h2ded and the WASM build
        #   utils           qbsp/light/vis/hcc et al
        #   hw_utils        the HexenWorld master server and rcon clients
        #   common          shared sources, pulled in by all three lists above
        #   scripts         utils/CMakeLists.txt reads it (mk_header.c)
        #   libs            vendored libTiMidity, compiled into the engine as
        #                   the tier-2 MIDI fallback codec when
        #                   USE_CODEC_TIMIDITY is on (always, for WASM and for
        #                   any build without FluidSynth) -- uhexen2-unvi
        #   oslibs          NOT optional despite no literal path in any
        #                   CMakeLists: engine/CMakeLists.txt reaches it as
        #                   ${UHEXEN2_TOP}/oslibs for the prebuilt Windows
        #                   SDL3 and codecs, and the win64 installPhase copies
        #                   SDL3.dll out of it by relative path
        #
        # Deliberately absent, and the reason it is safe: flake.nix,
        # flake.lock and shell-wasm.nix (a source copy of the recipe is not a
        # build input -- keeping them meant every flake edit rebuilt every
        # package from scratch), assets, patches, tools, h2patch, LICENSE and
        # the legacy top-level Makefile (not read by any CMake build).  The
        # release package reaches scripts/, oslibs/ and COPYING through
        # ${self}, which is the unfiltered flake source, so trimming here
        # cannot starve it.
        filteredSrc =
          let
            root = toString ./.;
            keep = [
              "CMakeLists.txt"
              "engine"
              "utils"
              "hw_utils"
              "common"
              "scripts"
              "oslibs"
              "libs"
            ];
          in pkgs.lib.cleanSourceWith {
            src = ./.;
            filter = path: type:
              let
                rel = pkgs.lib.removePrefix (root + "/") (toString path);
                top = builtins.head (pkgs.lib.splitString "/" rel);
              in
                builtins.elem top keep
                # Docs travelling inside a kept tree are still just docs.
                && !(type == "regular" && pkgs.lib.hasSuffix ".md" rel);
          };

        # The HexenC sources, and only those: input to `packages.gamecode`.
        #
        # A second filter rather than a "gamecode" entry in filteredSrc's
        # allowlist, and the asymmetry is the whole point.  hcc reads .hc
        # files; no CMake build does.  Sharing one source would make every
        # gamecode edit rebuild the engine, h2ded, both mingw cross builds,
        # the WASM build and the toolchain, and invalidate their cache
        # entries for every user -- while a .hc file cannot affect a single
        # byte of any of them.  Keeping the filters separate leaves the
        # engine's input hash untouched by anything under gamecode/.
        #
        # Scoped to gamecode/hc, not gamecode/: the siblings are res/, devel/,
        # mapfixes/, hc-unused/ and patch111/ (which carries xdelta binaries),
        # none of which hcc reads.
        gamecodeSrc =
          let
            root = toString ./.;
          in pkgs.lib.cleanSourceWith {
            name = "hexenwail-gamecode-src";
            src = ./.;
            filter = path: type:
              let
                rel = pkgs.lib.removePrefix (root + "/") (toString path);
              in
                rel == "gamecode" || rel == "gamecode/hc"
                || pkgs.lib.hasPrefix "gamecode/hc/" rel
                # The field-set gate and its golden tables (uhexen2-uhub).
                # Widening this filter means a checker edit rebuilds the
                # gamecode, which is correct -- the check is part of what
                # producing a progs.dat means -- and still leaves the engine's
                # filteredSrc hash untouched, which is the point of keeping the
                # two filters apart.
                || rel == "gamecode/fieldsets"
                || pkgs.lib.hasPrefix "gamecode/fieldsets/" rel
                # The changelog, because the ident-stamp gate below checks the
                # marker date against its newest fork entry.  A prose edit to
                # it now rebuilds the gamecode, which is the price of having
                # the changelog be load-bearing; the engine's filteredSrc is
                # still untouched, which is the property that matters.
                || rel == "gamecode/README"
                || rel == "tools"
                || rel == "tools/qcdis.py"
                || rel == "tools/check_progs_fields.py";
          };

        # Dr. MinGW's runtime -- the post-mortem handler we ship beside the
        # Windows binaries.  It is here rather than in nixpkgs because nixpkgs
        # does not package it, and it is worth the vendoring: we cross-compile
        # with mingw and so emit DWARF, which means the entire native Windows
        # crash-debugging stack (WinDbg, procdump, Windows Error Reporting
        # minidumps) cannot symbolize our binaries -- they all expect PDB.
        # Dr. MinGW reads DWARF, and its exchndl.dll writes a backtrace with
        # no debugger attached and nothing for the player to install.
        #
        # symsrv.dll and symsrv.yes are deliberately dropped: they exist to
        # pull symbols from Microsoft's server, which would have the handler
        # make a network request at crash time.  uhexen2-hger.
        drmingw = pkgs.stdenvNoCC.mkDerivation rec {
          pname = "drmingw-runtime";
          version = "0.9.13";

          src = pkgs.fetchurl {
            url = "https://github.com/jrfonseca/drmingw/releases/download/${version}/drmingw-${version}-win64.7z";
            hash = "sha256-okBKKuPgM4a1eb3ZjS048fauy/cWfZB+6v9E81CXoOA=";
          };

          nativeBuildInputs = [ pkgs.p7zip ];
          unpackPhase = "7z x $src";

          installPhase = ''
            mkdir -p $out/bin $out/licenses
            for dll in exchndl.dll mgwhelp.dll dbghelp.dll dbgcore.dll; do
              install -Dm755 drmingw-${version}-win64/bin/$dll $out/bin/$dll
            done
            install -Dm644 drmingw-${version}-win64/doc/LICENSE.txt \
              $out/licenses/LICENSE.drmingw
            install -Dm644 drmingw-${version}-win64/doc/LICENSE-libdwarf.txt \
              $out/licenses/LICENSE.libdwarf
            install -Dm644 drmingw-${version}-win64/doc/LICENSE-zlib.txt \
              $out/licenses/LICENSE.zlib
          '';

          meta.description = "Dr. MinGW post-mortem exception handler (runtime DLLs)";
        };

      in
      {
        packages = let
          # Shared build configuration for Linux builds
          linuxBuildAttrs = {
            pname = "hexenwail";
            inherit version;

            src = filteredSrc;

            nativeBuildInputs = with pkgs; [
              cmake
              pkg-config
            ];

            buildInputs = with pkgs; [
              sdl3
              libGL
              libvorbis      # Vorbis support
              libogg
              alsa-lib       # ALSA audio support
              fluidsynth     # MIDI synthesis
              libsndfile     # transitive dep of fluidsynth pkg-config
              flac           # transitive dep of libsndfile pkg-config
              libxmp         # XMP tracker music codec
              opusfile       # Opus codec support
              soundfont-fluid # GM soundfont for FluidSynth
            ];

            # CMake is in engine subdirectory
            preConfigure = ''
              cd engine
            '';

            cmakeFlags = [
              "-DUSE_CODEC_VORBIS=ON"
              "-DUSE_ALSA=ON"
              "-DSOUNDFONT_PATH=${pkgs.soundfont-fluid}/share/soundfonts/FluidR3_GM2-2.sf2"
            ];

            meta = with pkgs.lib; {
              description = "Hexenwail - modernized Hexen II engine (OpenGL 4.3)";
              longDescription = ''
                Hexenwail is a modern GL 4.3 fork of Hammer of Thyrion / uHexen2,
                the definitive Hexen II engine. SDL3, GLSL shaders, gamepad support,
                and a clean codebase for Linux and Windows.

                Note: This package only provides the game engine. You need the original
                game data files (pak0.pak, pak1.pak) from the commercial game to play.
              '';
              homepage = "https://github.com/hexenwail/hexenwail";
              license = licenses.gpl2Plus;
              platforms = platforms.linux;
              maintainers = [ ];
              mainProgram = "glhexen2";
            };
          };
        in {
          # OpenGL version (glhexen2) - NixOS build with Nix store rpaths
          # NOTE: Uses CMake build system
          nixos = pkgs.stdenv.mkDerivation (linuxBuildAttrs // {
            installPhase = ''
              runHook preInstall

              mkdir -p $out/bin

              # No $out/share/hexenwail here, deliberately.  This output
              # carries no gamecode -- referencing packages.gamecode from the
              # engine derivation would put it in this .drv's hash and make
              # every .hc edit rebuild the engine (uhexen2-r32l).  An EMPTY
              # share/hexenwail is worse than none: PR_FindBundleDir's layer 2
              # matches on the directory existing, so it would shadow layer 3's
              # BUNDLED_GAMECODE_DIR for any packager who set one.
              # packages.nixos-bundled composes this output with the gamecode
              # afterwards; that is what packages.default and apps.default
              # point at.  uhexen2-9die.

              # Install the OpenGL binary from CMake build directory
              install -Dm755 bin/glhexen2 $out/bin/glhexen2


              runHook postInstall
            '';
          });

          # The engine plus the gamecode it should run: what `nix run` and
          # `nix profile install` give you.  uhexen2-9die.
          #
          # Composed here rather than inside `nixos` because of uhexen2-r32l's
          # constraint -- an engine derivation that referenced ${gamecode}
          # would carry it in its .drv hash, and every .hc edit would rebuild
          # the engine, h2ded, both mingw cross builds and the WASM build.
          # Same trick `linux-fhs` already uses on this output.
          #
          # The binary is COPIED, not symlinked.  PR_FindBundleDir() resolves
          # the bundle relative to Sys_GetExeDir(), which reads
          # /proc/self/exe, and that resolves through a symlink to the real
          # target -- so a symlinkJoin would send the lookup back to the
          # gamecode-less `nixos` output and find nothing.
          #
          # Layout matches the release tree's: bin/glhexen2 with the gamecode
          # at ../share/hexenwail, which is PR_FindBundleDir()'s layer 2.
          # hw/ and siege/ are withheld here for the same reason as in the
          # release derivation -- see the note there.
          nixos-bundled =
            let
              nixosPkg = self.packages.${system}.nixos;
              gamecodePkg = self.packages.${system}.gamecode;
            in pkgs.runCommand "hexenwail-bundled-${nixosPkg.version}" {
              meta = nixosPkg.meta // {
                description = "${nixosPkg.meta.description} (with bundled gamecode)";
              };
              passthru = { inherit (nixosPkg) version; };
            } ''
              mkdir -p $out/bin
              cp ${nixosPkg}/bin/glhexen2 $out/bin/glhexen2
              chmod +w $out/bin/glhexen2

              install -Dm644 \
                ${gamecodePkg}/share/hexenwail/data1/progs.dat \
                ${gamecodePkg}/share/hexenwail/data1/progs2.dat \
                -t $out/share/hexenwail/data1
              install -Dm644 \
                ${gamecodePkg}/share/hexenwail/portals/progs.dat \
                -t $out/share/hexenwail/portals
            '';

          # Same composition for the dedicated server.  h2ded runs
          # PR_LoadProgs() exactly as the client does, so a bare .#h2ded has
          # the same gap -- and it is the engine used for headless gamecode
          # investigation, where running Raven's progs instead of ours would
          # quietly invalidate the run.  uhexen2-9die.
          h2ded-bundled =
            let
              h2dedPkg = self.packages.${system}.h2ded;
              gamecodePkg = self.packages.${system}.gamecode;
            in pkgs.runCommand "hexenwail-h2ded-bundled-${h2dedPkg.version}" {
              meta = h2dedPkg.meta // {
                description = "${h2dedPkg.meta.description} (with bundled gamecode)";
              };
              passthru = { inherit (h2dedPkg) version; };
            } ''
              mkdir -p $out/bin
              cp ${h2dedPkg}/bin/h2ded $out/bin/h2ded
              chmod +w $out/bin/h2ded

              install -Dm644 \
                ${gamecodePkg}/share/hexenwail/data1/progs.dat \
                ${gamecodePkg}/share/hexenwail/data1/progs2.dat \
                -t $out/share/hexenwail/data1
              install -Dm644 \
                ${gamecodePkg}/share/hexenwail/portals/progs.dat \
                -t $out/share/hexenwail/portals
            '';

          default = self.packages.${system}.nixos-bundled;

          # Dedicated server (h2ded) — headless, links only libm/libc.
          # A separate output rather than a flag on `nixos` so CI can build it
          # on every push: nothing else compiles the SERVERONLY half of the
          # shared sources, so without a gate it silently rots.
          h2ded = pkgs.stdenv.mkDerivation (linuxBuildAttrs // {
            pname = "hexenwail-h2ded";

            # The client's buildInputs are still needed even though h2ded links
            # none of them: engine/CMakeLists.txt looks up SDL3, OpenGL and ALSA
            # as REQUIRED for any Unix configure, not just the client target.
            cmakeFlags = linuxBuildAttrs.cmakeFlags ++ [ "-DBUILD_DEDICATED=ON" ];

            # Build only the server; the default target would also rebuild the
            # whole client, which .#nixos already covers.
            buildFlags = [ "h2ded" ];

            installPhase = ''
              runHook preInstall

              install -Dm755 bin/h2ded $out/bin/h2ded

              runHook postInstall
            '';

            meta = linuxBuildAttrs.meta // {
              description = "Hexenwail dedicated server (headless h2ded)";
              longDescription = ''
                Headless Hexen II server built from the shared engine sources
                with -DSERVERONLY: no renderer, video, sound or input.

                Note: you still need the original game data files (pak0.pak,
                pak1.pak) from the commercial game to host a server.
              '';
              mainProgram = "h2ded";
            };
          });

          # Map/model toolchain (utils/) and HexenWorld servers (hw_utils/).
          # Configures the repo root rather than engine/, with the engine
          # switched off: the tools are plain C with no external dependencies
          # at all, so this needs neither buildInputs nor pkg-config, and
          # skipping the engine keeps SDL3/OpenGL/ALSA out of the closure.
          # install(TARGETS) in the two CMakeLists means the stock installPhase
          # already places all 18 binaries in $out/bin.
          utils = pkgs.stdenv.mkDerivation {
            pname = "hexenwail-utils";
            inherit version;

            src = filteredSrc;

            nativeBuildInputs = [ pkgs.cmake ];

            cmakeFlags = [
              "-DBUILD_ENGINE=OFF"
              "-DBUILD_UTILS=ON"
            ];

            meta = with pkgs.lib; {
              description = "Hexen II map, model and server tools (qbsp, light, vis, hcc, ...)";
              longDescription = ''
                The Hexen II mapping and modding toolchain: qbsp, light, vis and
                jsh2colour for compiling maps, hcc/dhcc for HexenC bytecode,
                genmodel for .mdl files, pak/qfiles for archives, plus the
                HexenWorld master server and rcon clients.
              '';
              homepage = "https://github.com/hexenwail/hexenwail";
              license = licenses.gpl2Plus;
              platforms = platforms.linux;
              maintainers = [ ];
            };
          };

          # The HexenC gamecode, compiled to progs bytecode with hcc.
          #
          # This exists because gamecode fixes were rotting in place: nothing
          # in the build touched gamecode/, so a .hc edit that did not even
          # parse would land green, and every fix we made was inert for
          # players -- who run Raven's retail 1997 progs.dat.  Wiring it into
          # CI makes hcc the gate.  uhexen2-zmb3.
          #
          # `release` stages three of these files for the player to copy --
          # data1/progs.dat, data1/progs2.dat and portals/progs.dat, and not
          # hw/ or siege/ (uhexen2-8qp3).  See docs/GAMECODE.md for the licence
          # position, the precedence trace and the install path.
          #
          # All four trees, not just h2, because a fix typically lands in
          # three of them at once -- bb570666a (uhexen2-9r3n) touched h2, hw
          # and portals -- so gating only h2 would let the other two break
          # silently, which is the exact failure this output exists to stop.
          # They are seconds each and pull in no dependency beyond hcc.
          gamecode = pkgs.stdenvNoCC.mkDerivation {
            pname = "hexenwail-gamecode";
            inherit version;

            src = gamecodeSrc;

            nativeBuildInputs = [ self.packages.${system}.utils pkgs.python3 ];

            dontConfigure = true;

            # hcc resolves progs.src's first token against -src and writes the
            # .dat there, plus progdefs.h and files.dat beside it -- so it
            # needs a writable tree.  stdenv's unpack already gave us one (the
            # store copy is chmod u+w), hence no extra copy here.
            #
            # h2 alone omits -oi/-on: per gamecode/COMPILE those two make the
            # engine warn when loading pre-existing Hexen II saves.  The other
            # trees have no such legacy to protect.
            buildPhase = ''
              runHook preBuild

              hcc -src gamecode/hc/h2      -os
              hcc -src gamecode/hc/h2      -os -name progs2.src
              hcc -src gamecode/hc/portals -os -oi -on
              hcc -src gamecode/hc/hw      -os -oi -on
              hcc -src gamecode/hc/siege   -os -oi -on

              runHook postBuild
            '';

            # A savegame stores entity state by field NAME and carries no
            # fingerprint of the gamecode that wrote it, so an image whose
            # field set has drifted loads a stale save successfully and
            # silently missing state (uhexen2-acew).  The engine warns at load
            # time; this refuses to ship the divergence in the first place.
            #
            # The golden tables pin the field set against a checked-in copy,
            # because the image that matters most cannot be in the build:
            # Raven's retail PROGS.DAT is unfree and lives only in the player's
            # install.  As of 2026-08-15 h2.fields is exactly what retail
            # produces -- 497 fields, zero diff -- which is what makes
            # sv_gamecode 0 and 1 interchangeable.
            #
            # portals is checked separately against its own golden rather than
            # against h2: it is a different campaign in its own gamedir with
            # its own 511-field layout, and no savegame crosses between them.
            # hw and siege are HexenWorld images with no savegames of this
            # kind, so they are not gated.
            doCheck = true;
            checkPhase = ''
              runHook preCheck

              python3 tools/check_progs_fields.py \
                --golden gamecode/fieldsets/h2.fields \
                gamecode/hc/h2/progs.dat gamecode/hc/h2/progs2.dat

              python3 tools/check_progs_fields.py \
                --golden gamecode/fieldsets/portals.fields \
                gamecode/hc/portals/progs.dat

              # The marker PR_ClassifyGamecode() looks up to tell our gamecode
              # apart from Raven's (uhexen2-8r3e).  Losing it does not fail a
              # build or a map load -- the engine simply reports our own progs
              # as "Third-party" -- so nothing else would ever notice.  The
              # realistic ways to lose it are dropping ident.hc from a
              # progs.src or renaming the function, and both are caught here.
              #
              # This gate cannot see the engine's half of the contract:
              # GAMECODE_SENTINEL lives in engine/h2shared/pr_edict.c, which is
              # deliberately not an input to this derivation (uhexen2-r32l), so
              # the name is repeated below rather than shared.  A three-way
              # rename still needs a human.
              # ANCHORED.  qcdis --list takes a regex and applies re.search, so
              # a bare name also matches HexenwailGamecodeXX and any other
              # superstring -- which is exactly the rename this gate exists to
              # catch, and it passed until the anchors went in.
              # The stamp is a hand-typed constant, so the real hazard is not a
              # typo but a gamecode change that forgets to restamp -- leaving a
              # confident wrong date, which is worse than no date at all.  The
              # fork's policy is already that every divergence gets a dated
              # gamecode/README entry, so requiring the two to agree makes the
              # build fail at exactly the moment a change is recorded without
              # restamping.  Fork entries are "YYYY-MM-DD [uhexen2-xxxx]:"; the
              # pre-fork history uses "(1.29c)"-style parens and is not matched.
              #
              # Checking every image against that one expected value also gets
              # "all five agree" for free -- one tree restamped and the others
              # not would ship five images claiming different revisions, with
              # the engine reporting whichever the player happened to load.
              newest=$(grep -oE '^[0-9]{4}-[0-9]{2}-[0-9]{2} \[' gamecode/README \
                       | cut -c1-10 | sort -r | head -1 | tr -d -)
              if [ -z "$newest" ]; then
                echo "ERROR: no dated fork entry found in gamecode/README." >&2
                exit 1
              fi
              want="HexenwailGamecode_$newest"

              for p in gamecode/hc/h2/progs.dat gamecode/hc/h2/progs2.dat \
                       gamecode/hc/portals/progs.dat \
                       gamecode/hc/hw/hwprogs.dat gamecode/hc/siege/hwprogs.dat; do
                got=$(python3 tools/qcdis.py "$p" \
                        --list '^HexenwailGamecode_[0-9]{8}$' | awk '{print $1}')
                if [ -z "$got" ]; then
                  echo "ERROR: $p carries no HexenwailGamecode_YYYYMMDD marker." >&2
                  echo "       See gamecode/hc/*/ident.hc and its progs.src entry." >&2
                  exit 1
                fi
                if [ "$got" != "$want" ]; then
                  echo "ERROR: $p is stamped $got but the newest" >&2
                  echo "       gamecode/README entry is dated $newest." >&2
                  echo "       Restamp gamecode/hc/*/ident.hc, or date the entry." >&2
                  exit 1
                fi
              done

              runHook postCheck
            '';

            # Laid out as gamedirs under a basedir, matching packages.demodata,
            # so the tree can be dropped straight onto an install:
            #   cp -r result/share/hexenwail/data1/. ~/.hexen2/data1/
            #
            # hw/ and siege/ are HexenWorld progs and this fork builds no
            # HexenWorld engine, so they are compile-gate artefacts today.
            # They are installed anyway rather than discarded: a HexenWorld
            # server operator can use them, and dropping them would invite
            # someone to "simplify" the build steps that produce them.
            installPhase = ''
              runHook preInstall

              install -Dm644 gamecode/hc/h2/progs.dat \
                gamecode/hc/h2/progs2.dat -t $out/share/hexenwail/data1
              install -Dm644 gamecode/hc/portals/progs.dat \
                -t $out/share/hexenwail/portals
              install -Dm644 gamecode/hc/hw/hwprogs.dat \
                -t $out/share/hexenwail/hw
              install -Dm644 gamecode/hc/siege/hwprogs.dat \
                -t $out/share/hexenwail/siege

              runHook postInstall
            '';

            meta = with pkgs.lib; {
              description = "Hexen II gamecode (progs.dat) built from the HexenC sources";
              longDescription = ''
                The uHexen2 HexenC gamecode compiled with hcc: progs.dat and
                progs2.dat for Hexen II, progs.dat for the Portal of Praevus
                mission pack, and hwprogs.dat for HexenWorld and Siege.

                This carries the Hammer of Thyrion gamecode fixes through
                1.29c plus Hexenwail's own, none of which are in the retail
                1997 progs.dat that players otherwise run.

                Release bundles stage the Hexen II and Portals files under
                gamecode/ for the player to copy; hw/ and siege/ are built as a
                compile gate only and are not shipped. To install from a source
                checkout instead:

                  nix build .#gamecode
                  cp result/share/hexenwail/data1/progs.dat ~/.hexen2/data1/

                See docs/GAMECODE.md before doing so -- a mod's own progs.dat
                takes precedence, but a stale copy in data1/ is invisible and
                permanent until removed by hand.
              '';
              homepage = "https://github.com/hexenwail/hexenwail";
              license = licenses.gpl2Plus;
              platforms = platforms.linux;
              maintainers = [ ];
            };
          };

          # OpenGL version for standard FHS Linux systems (non-NixOS)
          # Bundles shared libraries so it runs on any distro without nix
          linux-fhs = let
            nixosPkg = self.packages.${system}.nixos;
            # TimGM6mb (Tim Brechbill, GPL-2, ~6 MB) — small enough to bundle in
            # a downloadable release, unlike FluidR3 (~142 MB).  Same soundfont
            # the Flatpak ships.  Debian upstream tarball; hash is the tarball's.
            # deb.debian.org serves only what is currently in the archive, so
            # that URL 404s the moment this package is superseded or dropped —
            # and it would take the release build with it.  snapshot.debian.org
            # is Debian's permanent archive and keeps the 1.3 tarball at a
            # fixed timestamp forever.  fetchurl tries each URL in order and
            # the result is content-addressed by the hash below, so the mirror
            # can only ever serve the same bytes (verified: the snapshot copy
            # hashes identically).  Same primary/fallback approach already used
            # for the Xiph downloads in the Flatpak manifest.  uhexen2-bbul.
            timgmTar = pkgs.fetchurl {
              urls = [
                "https://deb.debian.org/debian/pool/main/t/timgm6mb-soundfont/timgm6mb-soundfont_1.3.orig.tar.gz"
                "https://snapshot.debian.org/archive/debian/20240101T000000Z/pool/main/t/timgm6mb-soundfont/timgm6mb-soundfont_1.3.orig.tar.gz"
              ];
              sha256 = "af8f3a00e416dfb262bcaa904a1c84df04a51b72bbc1313aed012bc754bdf99b";
            };
            runtimeLibs = with pkgs; [
              sdl3
              libvorbis
              libogg
              fluidsynth
              libsndfile
              flac
              libxmp
              opusfile
              libopus
              alsa-lib
            ];
          in pkgs.runCommand "hexenwail-linux-fhs-${nixosPkg.version}" {
            nativeBuildInputs = [ pkgs.patchelf ];
          } ''
            mkdir -p $out/bin $out/lib

            cp ${nixosPkg}/bin/glhexen2 $out/bin/glhexen2
            chmod +w $out/bin/glhexen2

            # Bundle a GM soundfont next to the binary so MIDI music works on
            # machines with no system soundfont (the compile-time SOUNDFONT_PATH
            # points into /nix/store, which is absent off-NixOS).  find_soundfont
            # probes <exe dir>/soundfont.sf2 first.
            tar xzf ${timgmTar}
            cp -L timgm6mb-soundfont_1.3/TimGM6mb.sf2 $out/bin/soundfont.sf2

            # Bundle shared libraries
            for lib in ${pkgs.lib.concatMapStringsSep " " (l: "${l}/lib") runtimeLibs}; do
              for so in "$lib"/*.so "$lib"/*.so.*; do
                [ -e "$so" ] && cp -nL "$so" $out/lib/ 2>/dev/null || true
              done
            done
            chmod +w $out/lib/*

            # Set FHS interpreter and relative rpaths
            patchelf \
              --set-interpreter /lib64/ld-linux-x86-64.so.2 \
              --set-rpath "\$ORIGIN/../lib" \
              $out/bin/glhexen2
            for so in $out/lib/*.so $out/lib/*.so.*; do
              [ -f "$so" ] && patchelf --set-rpath "\$ORIGIN" "$so" 2>/dev/null || true
            done
          '';


          # Windows 64-bit build.  Ships with its debug symbols kept, which is
          # a deliberate choice rather than an oversight: -g does not affect
          # codegen (verified -- .text hashes identical with and without it),
          # and the loader never maps DWARF sections, so players get exactly
          # the same machine code at exactly the same speed.  The only cost is
          # ~4.2 MB of download.
          #
          # Paying that once means there is no second "debug build" to talk
          # anyone into downloading: every crash any player ever hits comes
          # back as a report naming source files and line numbers.  A separate
          # symbol build was built first and dropped for exactly that reason.
          # uhexen2-hger.
          win64 = pkgsCross64.stdenv.mkDerivation {
            # Explicit `name` rather than pname+version, to fix the build-log
            # prefix.  nixpkgs always appends the host triple on cross builds
            # (make-derivation.nix, `hostSuffix`) — there is no opt-out — but
            # *where* it lands depends on which attrs you set:
            #
            #   pname+version -> hexenwail-x86_64-w64-mingw32-0.7.9-beta.r9
            #   name          -> hexenwail-win64-0.7.9-beta.r9-x86_64-w64-mingw32
            #
            # nix's log prefix truncates at the first "-<digit>" component, so
            # the first form logs as "hexenwail-x86_64-w64-mingw32>" (reads as
            # a typo: 64, 64, 32) and the second as "hexenwail-win64>".
            # The store path is longer either way; the prefix is what we want.
            name = "hexenwail-win64-${version}";

            src = filteredSrc;

            nativeBuildInputs = with pkgs; [
              cmake
              pkg-config
              removeReferencesTo
            ];

            buildInputs = with pkgsCross64; [
              windows.pthreads
              libvorbis      # Ogg Vorbis codec
              libogg         # Ogg container (shared by vorbis + opus)
              libxmp         # XMP tracker music codec
              opusfile       # Opus codec support
            ];

            # CMake is in engine subdirectory
            preConfigure = ''
              cd engine
            '';

            cmakeFlags = [
              "-DUSE_CODEC_VORBIS=ON"
              "-DUSE_CODEC_OPUS=ON"
              "-DUSE_CODEC_XMP=ON"
              "-DUSE_DEBUGINFO=ON"
            ];

            # Tidy the paths the DWARF we now ship records.  Mapping the
            # source root makes a crash report read
            # "/hexenwail/engine/hexen2/cl_parse.c" instead of exposing the
            # build sandbox, and -gno-record-gcc-switches drops the
            # DW_AT_producer command line, which is just the -I flags.
            #
            # The replacement must stay absolute (leading slash).  Mapping to
            # a bare "hexenwail" makes every DW_AT_name look relative, and
            # addr2line then joins it onto DW_AT_comp_dir -- which was mapped
            # the same way -- yielding the doubled nonsense
            # "hexenwail/engine/build/hexenwail/engine/hexen2/cl_parse.c".
            #
            # -fdebug-prefix-map rather than -ffile-prefix-map on purpose: the
            # latter also rewrites __FILE__, which would change the text of
            # Sys_Error messages.  Note this only renames the /nix/store
            # prefix -- the store hashes survive it, which is what postFixup
            # below has to deal with.  uhexen2-hger.
            env.NIX_CFLAGS_COMPILE = builtins.concatStringsSep " " [
              "-gno-record-gcc-switches"
              "-fdebug-prefix-map=/nix/store=/nixpkgs"
              "-fdebug-prefix-map=/build/source=/hexenwail"
            ];

            installPhase = ''
              runHook preInstall

              mkdir -p $out/bin

              # Install the Windows executable
              install -Dm755 bin/glh2.exe $out/bin/glh2.exe

              # Dr. MinGW's post-mortem handler.  engine/hexen2/sys_win.c
              # LoadLibraryA()s exchndl.dll as the first thing WinMain does,
              # so shipping these four DLLs is the entire crash-reporting
              # setup -- a crash drops a symbolized glh2.RPT next to the
              # binary without the player installing a debugger.  uhexen2-hger.
              install -Dm755 ${drmingw}/bin/*.dll -t $out/bin/

              # Install DLLs from build output (MinGW runtime)
              for dll in bin/*.dll; do
                [ -f "$dll" ] && install -Dm755 "$dll" $out/bin/
              done

              # Bundle SDL3
              install -Dm755 ../../oslibs/windows/SDL3/lib64/SDL3.dll $out/bin/SDL3.dll

              # Bundle codec DLLs from nix cross-compiled packages
              # libogg exports as "ogg.dll" in nix meson builds
              install -Dm755 ${pkgsCross64.libogg}/bin/libogg.dll $out/bin/ogg.dll
              install -Dm755 ${pkgsCross64.libvorbis}/bin/libvorbis-0.dll $out/bin/
              install -Dm755 ${pkgsCross64.libvorbis}/bin/libvorbisfile-3.dll $out/bin/
              install -Dm755 ${pkgsCross64.opusfile}/bin/libopusfile-0.dll $out/bin/
              install -Dm755 ${pkgsCross64.libopus}/bin/libopus-0.dll $out/bin/
              install -Dm755 ${pkgsCross64.libxmp}/bin/libxmp.dll $out/bin/

              # Bundle MinGW runtime DLLs if present
              for dll in libgcc_s_seh-1.dll libwinpthread-1.dll libstdc++-6.dll; do
                found=$(find ${pkgsCross64.stdenv.cc.cc} -name "$dll" 2>/dev/null | head -1)
                if [ -n "$found" ]; then
                  install -Dm755 "$found" $out/bin/"$dll"
                fi
              done

              runHook postInstall
            '';

            dontStrip = true;
            postFixup = ''
              for f in $out/bin/*.dll; do
                if [ -L "$f" ]; then
                  cp -L "$f" "$f.tmp" && mv "$f.tmp" "$f"
                fi
              done

              # Skip Dr. MinGW's runtime.  dbghelp.dll and dbgcore.dll in that
              # set are redistributed *Microsoft* binaries, and running GNU
              # strip across a signed MS PE is a good way to ship a DLL that
              # no longer loads -- which would silently disable exactly the
              # crash reporting we just added.  All four are shipped stripped
              # upstream anyway, so there is nothing to gain.
              for f in $out/bin/*.dll; do
                case "''${f##*/}" in
                  exchndl.dll|mgwhelp.dll|dbghelp.dll|dbgcore.dll) continue ;;
                esac
                $STRIP -S -p "$f" 2>/dev/null || true
              done

              # GCC bakes the absolute path of every header it read, and of
              # its own include dir, into the DWARF.  A Windows .exe has no
              # runtime dependency on any of them -- but nix scans the output
              # for store hashes, finds those, and concludes otherwise: the
              # closure went from 7.8 MiB to 703 MiB, dragging the mingw
              # toolchain and five -dev outputs behind a binary that links
              # none of them.  Players never see it; the binary cache would
              # eat ~700 MiB per build.
              #
              # remove-references-to overwrites each hash in place with a dead
              # one of the same length, so the PE layout is untouched and the
              # paths become inert text.  Anchoring the scan to the /nixpkgs
              # prefix that -fdebug-prefix-map produces is what keeps it from
              # matching a 32-byte run inside .text and corrupting code.  Our
              # own source paths are not store paths and so are untouched --
              # they are what makes the reports readable.  uhexen2-hger.
              for h in $(grep -oaE '/nixpkgs/[0-9a-df-np-sv-z]{32}-' \
                           $out/bin/glh2.exe | cut -d/ -f3 | cut -d- -f1 | sort -u); do
                remove-references-to -t "$NIX_STORE/$h-x" $out/bin/glh2.exe
              done
            '';
            # Note glh2.exe is deliberately absent from the strip loop above:
            # its DWARF is the payload, not waste.  Stripping it would turn
            # every future crash report back into raw hex addresses.

            meta = with pkgs.lib; {
              description = "Hexenwail - modernized Hexen II engine (OpenGL 4.3, Windows 64-bit)";
              homepage = "https://github.com/hexenwail/hexenwail";
              license = licenses.gpl2Plus;
              platforms = platforms.windows;
              maintainers = [ ];
            };
          };

          # Dedicated server, cross-compiled for Windows.  Its own output
          # because the mingw toolchain is already here for .#win64, and the
          # WIN32 half of the h2ded target has nothing else to prove it builds.
          h2ded-win64 = pkgsCross64.stdenv.mkDerivation {
            # Explicit `name` — see .#win64 above.
            name = "hexenwail-h2ded-win64-${version}";

            src = filteredSrc;

            nativeBuildInputs = with pkgs; [
              cmake
              pkg-config
            ];

            # No buildInputs at all: the Windows server links only ws2_32,
            # winmm and the mingw runtime, and the Windows configure takes SDL3
            # from oslibs rather than from a package.

            preConfigure = ''
              cd engine
            '';

            cmakeFlags = [ "-DBUILD_DEDICATED=ON" ];

            # Only the server target; the client .exe is .#win64's job.
            buildFlags = [ "h2ded" ];

            installPhase = ''
              runHook preInstall

              install -Dm755 bin/h2ded.exe $out/bin/h2ded.exe

              runHook postInstall
            '';

            meta = with pkgs.lib; {
              description = "Hexenwail dedicated server (headless h2ded, Windows 64-bit)";
              homepage = "https://github.com/hexenwail/hexenwail";
              license = licenses.gpl2Plus;
              platforms = platforms.windows;
              maintainers = [ ];
            };
          };

          # Map/model toolchain for Windows.  The tools are plain C linking
          # only the mingw runtime (and ws2_32 for the HexenWorld servers), so
          # like h2ded-win64 this needs no buildInputs.  uhexen2-4xd7.
          utils-win64 = pkgsCross64.stdenv.mkDerivation {
            # Explicit `name` — see .#win64 above.
            name = "hexenwail-utils-win64-${version}";

            src = filteredSrc;

            nativeBuildInputs = [ pkgs.cmake ];

            cmakeFlags = [
              "-DBUILD_ENGINE=OFF"
              "-DBUILD_UTILS=ON"
            ];

            meta = with pkgs.lib; {
              description = "Hexen II map, model and server tools (Windows 64-bit)";
              homepage = "https://github.com/hexenwail/hexenwail";
              license = licenses.gpl2Plus;
              platforms = platforms.windows;
              maintainers = [ ];
            };
          };

          # WebAssembly / Emscripten build
          # NOTE: WASM builds require network access for Emscripten SDL3 port
          # Quick fix (temporary): Use shell-wasm.nix for interactive dev builds
          # Long-term: See issue uhexen2-1z31 for reproducible solution
          wasm = pkgs.stdenv.mkDerivation {
            pname = "hexenwail-wasm";
            inherit version;

            src = filteredSrc;

            nativeBuildInputs = with pkgs; [
              emscripten
              cmake
              pkg-config
              nodejs
              sdl3
            ];

            # Emscripten-specific setup
            preConfigure = ''
              export EM_CACHE="''${EM_CACHE:-.emcache}"
              export EM_CONFIG="''${EM_CONFIG:-.emscripten}"
            '';

            # Use Emscripten's CMake toolchain
            configurePhase = ''
              mkdir -p build
              cd build
              emcmake cmake \
                -DCMAKE_BUILD_TYPE=Release \
                -DUSE_CODEC_VORBIS=OFF \
                -DUSE_ALSA=OFF \
                -DUSE_SDL3_STATIC=ON \
                -DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE \
                ../engine
            '';

            buildPhase = ''
              emmake make -j1 VERBOSE=1
            '';

            installPhase = ''
              mkdir -p $out
              cp bin/hexenwail.html $out/index.html
              cp bin/hexenwail.js $out/
              cp bin/hexenwail.wasm $out/
              cp bin/hexenwail.worker.js $out/ 2>/dev/null || true
            '';

            meta = with pkgs.lib; {
              description = "Hexenwail - WebAssembly browser build (GL ES 3.0)";
              longDescription = ''
                Hexenwail WebAssembly / Emscripten build for browser gameplay.
                Requires users to provide game data files (pak0.pak, pak1.pak).

                Note: Pure Nix flake builds cannot fetch Emscripten ports due to
                sandbox restrictions. For WASM development, use:
                  nix develop -f shell-wasm.nix
              '';
              homepage = "https://github.com/hexenwail/hexenwail";
              license = licenses.gpl2Plus;
              platforms = platforms.linux;
            };
          };

          # Raven's free Nov 1997 three-level demo data, fetched from the
          # uHexen2 project.  The declarative counterpart to `nix run
          # .#get-demo`: same tarball, same sha256, same "keep only data1"
          # rule, but hash-pinned and usable as a store path:
          #
          #   nix build .#demodata
          #   nix run .#default -- -basedir ./result/share/hexenwail
          #
          # DO NOT WIRE THIS INTO ANYTHING WE PUBLISH.  uhexen2-3vmk settled
          # that the only licence shipping with this data is Activision's
          # RETAIL EULA -- it forbids transferring copies, and a search for a
          # demo-specific grant came back empty.  The line that decision draws
          # is: pointing a user at uHexen2's URL is not distribution, serving
          # them the bytes is.  A fetchurl the *user* evaluates stays on the
          # right side of it; a copy in hexenwail.cachix.org would not.  So
          # this output must never be referenced by `release`, never appear in
          # a workflow (ci.yml/release.yml build named packages one by one --
          # keep it that way, and never add a build-everything sweep), and
          # never be pushed to the binary cache.  hydraPlatforms = [] and the
          # unfree licence below are the guards; both are load-bearing.
          #
          # licenses.unfree, deliberately not unfreeRedistributable: the
          # latter asserts a redistribution grant, and per uhexen2-3vmk no
          # such grant is known to exist.  (uhexen2-118f)
          demodata = pkgs.stdenvNoCC.mkDerivation {
            pname = "hexen2-demodata";
            version = "1997-11";

            src = pkgs.fetchurl {
              # mirror:// first so a dead SourceForge mirror rotates instead
              # of failing the build; the direct URL the get_demo scripts use
              # is kept as the last resort.  Content-addressed by the hash, so
              # no mirror can serve different bytes.
              urls = [
                "mirror://sourceforge/uhexen2/Hexen2Demo-Nov.1997/hexen2demo_nov1997-linux-i586.tgz"
                "https://downloads.sourceforge.net/project/uhexen2/Hexen2Demo-Nov.1997/hexen2demo_nov1997-linux-i586.tgz"
              ];
              # Recorded in assets/demo/README.md alongside the provenance.
              hash = "sha256-LfFc3gEot6A25xmV4GjKhT8Tvo4rWRyqwUACXWZkP8A=";
            };

            sourceRoot = "hexen2demo_nov1997";

            dontConfigure = true;
            dontBuild = true;

            # The tarball also carries an i586 Hammer of Thyrion build
            # (glhexen2, hexen2, h2ded, hexen2.svga).  Those are GPL and
            # harmless, but they are a 1997 32-bit engine we have no use for,
            # so only data1 is installed.
            installPhase = ''
              runHook preInstall

              mkdir -p $out/share/hexenwail
              cp -r data1 $out/share/hexenwail/data1

              # Keep the terms with the bytes -- SUBLICENSE.doc is the whole
              # reason this package is marked unfree, so shipping the data
              # without it would hide the one document that matters.
              install -Dm644 SUBLICENSE.doc DEMO.TXT docs/ABOUT \
                -t $out/share/doc/hexen2-demodata

              runHook postInstall
            '';

            # 13 MB over the wire, then a copy; not worth a remote builder.
            preferLocalBuild = true;

            meta = with pkgs.lib; {
              description = "Hexen II three-level demo data (Raven, Nov 1997)";
              longDescription = ''
                The data1/ directory from the free three-level Hexen II demo
                Raven released in November 1997, as repackaged by the
                Hammer of Thyrion (uHexen2) project. Enough content to run
                Hexenwail without a retail copy of the game:

                  nix build .#demodata
                  nix run .#default -- -basedir ./result/share/hexenwail

                Copying data1/ from a GOG, Steam or disc installation over it
                unlocks the full game.

                Hexenwail neither hosts nor relicenses this data: building
                this package downloads it from uHexen2's SourceForge project
                to your machine. See assets/demo/README.md for provenance and
                terms, and the SUBLICENSE.doc installed into share/doc.
              '';
              homepage = "https://sourceforge.net/projects/uhexen2/files/Hexen2Demo-Nov.1997/";
              license = licenses.unfree;
              platforms = platforms.all;
              # Never build this on a shared builder or push it to a cache.
              hydraPlatforms = [ ];
              sourceProvenance = with sourceTypes; [ binaryBytecode ];
            };
          };

          # Release package - builds all platforms together
          release = pkgs.runCommand "hexenwail-release-${version}" {
            meta = with pkgs.lib; {
              description = "Hexenwail - Multi-platform release bundle";
              homepage = "https://github.com/hexenwail/hexenwail";
              license = licenses.gpl2Plus;
              platforms = platforms.linux;
            };
          } ''
            mkdir -p $out/release

            # Linux portable (FHS binary, runs on any distro)
            # lib/ is not optional here: linux-fhs patchelfs the binary to
            # rpath $ORIGIN/../lib and bundles ~36 shared objects there, so
            # shipping bin/ alone produced a "portable" build that resolved
            # its rpath to a directory that did not exist and failed to start
            # on any machine without those libraries system-wide — i.e. on
            # exactly the distros the portable build is for. uhexen2-3aet.
            mkdir -p $out/release/linux-x86_64
            cp -r ${self.packages.${system}.linux-fhs}/bin $out/release/linux-x86_64/
            cp -rL ${self.packages.${system}.linux-fhs}/lib $out/release/linux-x86_64/

            # No linux-x86_64-nixos/ tree.  .#nixos leaves the binary's RPATH
            # pointing at absolute /nix/store paths -- SDL3, libglvnd, ALSA,
            # fluidsynth and glibc itself -- so a copy of it is startable only
            # on a machine that already has those exact store paths, which no
            # download can promise, not even on another NixOS box with a
            # different nixpkgs pin.  Publishing it would ship a binary that
            # fails to launch for everyone who is not the builder.  NixOS users
            # are served by the flake instead: `nix run github:bobberb/hexenwail`
            # builds against their own nixpkgs and gets correct rpaths.
            # uhexen2-2tia.

            # Windows 64-bit (dereference symlinks so DLLs are real files)
            mkdir -p $out/release/windows-x86_64
            cp -rL ${self.packages.${system}.win64}/bin $out/release/windows-x86_64/

            # Demo fetch helper, beside the binary in every platform dir.
            #
            # The engine's no-data error names this script, and until it
            # shipped here that instruction was answerable only from a source
            # checkout -- i.e. by developers, not by the people who actually
            # hit the error.  It is a downloader, not game content: nothing
            # Raven owns is in this bundle.  uhexen2-49ep.
            install -Dm755 ${self}/scripts/get_demo.sh $out/release/linux-x86_64/get_demo.sh
            install -Dm644 ${self}/scripts/get_demo.ps1 $out/release/windows-x86_64/get_demo.ps1
            install -Dm755 ${self}/scripts/get_demo.cmd $out/release/windows-x86_64/get_demo.cmd

            # Compiled gamecode.  uhexen2-8qp3.
            #
            # One shared directory, not a copy per platform: progs bytecode is
            # platform-independent, so three copies would add ~5.7 MB to the
            # download to say the same thing three times.  The tree under
            # gamecode/ names the gamedirs the files belong to -- data1/ and
            # portals/ -- rather than flattening the three files together, so
            # that wherever they are being copied to, the destination is
            # self-evident.
            #
            # Since uhexen2-xsmc the player copies nothing on either platform:
            # the engine loads this gamecode from beside its own executable.
            # On Windows this very directory is what it finds (release.yml
            # flattens that zip, leaving gamecode/ next to glh2.exe, i.e. the
            # first lookup layer); on Linux it finds the per-platform copy
            # installed below.  What survives here is the hand-install case --
            # feeding these files to a DIFFERENT engine -- which is the only
            # remaining reason to copy them anywhere.  gamecode/README.txt
            # frames it that way and says to back up first.
            #
            # Staged, NOT extracted over the player's data1/.
            # Retail ships progs.dat as a LOOSE file and the paks contain no
            # copy of it (verified against a retail install: pak0/pak1/pak3
            # hold zero .dat entries), so an overlay that lands directly in
            # data1/ overwrites the player's only copy of Raven's gamecode,
            # irreversibly and without asking.  On Windows that happens on
            # unzip, before anyone has read a word of documentation.  The
            # copy stays a deliberate act; gamecode/README.txt says to back up
            # first.  docs/GAMECODE.md has the precedence trace behind this.
            #
            # portals/ is not optional.  Priority is mod > portals > data1 and
            # the unit of priority is a whole gamedir, so with the mission pack
            # active a shipped data1/progs.dat is never read -- shipping only
            # h2 would miss every Portals player.
            #
            # hw/ and siege/ are built (they gate compile errors) but withheld:
            # no retail HexenWorld bytecode exists to compare them against and
            # this fork builds no HexenWorld engine.  Deferred to uhexen2-nr9l.
            # Named one by one rather than copying the tree so that stays true
            # by construction, and so a vanished path fails the build.
            install -Dm644 \
              ${self.packages.${system}.gamecode}/share/hexenwail/data1/progs.dat \
              ${self.packages.${system}.gamecode}/share/hexenwail/data1/progs2.dat \
              -t $out/release/gamecode/data1
            install -Dm644 \
              ${self.packages.${system}.gamecode}/share/hexenwail/portals/progs.dat \
              -t $out/release/gamecode/portals

            cat > $out/release/gamecode/README.txt <<'EOF'
Hexenwail compiled gamecode (progs.dat)
=======================================

There is nothing to install.  The engine already loads these.

They are the Hexen II game logic, rebuilt from the HexenC sources in this
project.  They carry bug fixes that are not in Raven's 1997 progs.dat -- most
notably a fix for dropped backpacks silently vanishing in co-op and
deathmatch.  Nothing else in this download changes game behaviour; these files
do.

Hexenwail carries its own copy of them next to the engine and prefers it to
the one in your Hexen II folder.  That is true on Linux and on Windows alike,
straight out of the zip: nothing is copied, nothing of yours is overwritten,
and there is no step you have missed.

So this gamecode/ folder is not a job waiting for you.  On Windows it IS the
copy the engine loads -- it sits beside glh2.exe, which is where the engine
looks first.  On Linux the engine loads its own from the platform directory
and this folder is a spare.  Either way, read on only if you want to check
what you are running, turn it off, or use these files somewhere else.

WHICH GAMECODE AM I RUNNING?
----------------------------
The engine says so.  Whenever it loads gamecode it prints one line to the
console naming the exact file:

  Gamecode: progs.dat from <full path to the file> (<version>, file crc <n>)

If that path has gamecode or share/hexenwail in it, you are running ours.  If
it points inside your Hexen II folder, you are running the one that came with
the game.  Quote the whole line in bug reports; the crc identifies the file
exactly, which a filename cannot.

One exception worth knowing: our gamecode is built from the v1.11 sources, so
the engine declines to substitute it on the shareware demo, the OEM release
and mix-and-match installs -- those are different versions of the game.  There
you get the gamecode your install came with, and the line above will say so.

TURNING IT OFF
--------------
Launch the engine with -vanillaprogs.  It then ignores its own copy entirely
and uses whatever gamecode your Hexen II install provides.

Nothing is moved or deleted, so this is a per-launch decision -- drop the
switch and you are back on ours.  Use it when you want Raven's 1997 behaviour,
and when you are reporting a bug and want to say whether it happens both ways.

RUNNING SOME OTHER GAMECODE
---------------------------
A translation, a balance patch, your own build.

Linux, macOS, BSD -- put it in your Hexen II user directory.  The engine
creates it the first time you run it, and it is where your config and
savegames already live:

  ~/.hexen2/data1/progs.dat
  ~/.hexen2/data1/progs2.dat
  ~/.hexen2/portals/progs.dat

The order is: your user directory beats the engine's own copy, which beats
your Hexen II folder.  ~/.hexen2 is therefore the one place that wins
outright, and it is why a progs.dat dropped into <your Hexen II folder>/data1/
has no effect -- the engine's copy is preferred to it.

Two more reasons it is the right place, both about being able to undo it:

  - Nothing of Raven's is overwritten.  Retail Hexen II keeps progs.dat as a
    loose file and there is NO copy inside pak0.pak/pak1.pak/pak3.pak to fall
    back on, so overwriting it in the game folder cannot be undone.

  - Uninstalling is deleting files you put there yourself, in a directory you
    own.  The game folder is never touched, so there is nothing to restore.

Windows -- there is no user directory, so replace the files inside the
gamecode\ folder beside glh2.exe instead:

  gamecode\data1\progs.dat
  gamecode\data1\progs2.dat
  gamecode\portals\progs.dat

Those are our files, not Raven's, so nothing irreplaceable is at risk --
re-extracting the zip puts them back.

USING THESE WITH A DIFFERENT ENGINE
-----------------------------------
This is the one case that calls for copying anything.  The original glhexen2,
or another port, will not know to look beside its own executable, so it wants
the files in the game folder:

  gamecode\data1\progs.dat    ->  <your Hexen II folder>\data1\progs.dat
  gamecode\data1\progs2.dat   ->  <your Hexen II folder>\data1\progs2.dat
  gamecode\portals\progs.dat  ->  <your Hexen II folder>\portals\progs.dat

BACK UP FIRST.  That overwrites Raven's files, and as above no .pak holds a
copy to fall back on, so your backup is the only way back.  Put your existing
data1\progs.dat, data1\progs2.dat and portals\progs.dat somewhere safe before
you copy over them.  To undo it, restore those backups -- nothing else is
needed.

None of this is required to play Hexenwail, and doing it changes what that
other engine runs, not what Hexenwail runs.

Whichever engine you are feeding, copy progs.dat and progs2.dat TOGETHER.
Five maps use progs2.dat and the rest use progs.dat: rider1a, rider2c, meso9,
romeric6 and eidolon -- the boss arenas at the end of each hub, and Eidolon's
lair.  Installing one file without the other leaves the game running new
gamecode on those five maps and 1997 gamecode everywhere else.

Include portals/progs.dat if you have the Portal of Praevus mission pack.
When the mission pack is active it takes priority over data1 completely, so
without this file you get none of the fixes.

WHAT CHANGES
------------
Crusader's Glyph of the Ancients (Portal of Praevus).  The glyph detonates on
whoever touches it for real damage -- 50 in deathmatch, 37 otherwise -- and
that includes YOU, the caster.  Do not walk into your own glyph.

This is Raven's original behaviour, so nothing about the glyph changes if you
are coming from a stock progs.dat.  It is called out because Hexenwail's own
gamecode briefly did something else: builds made from source for a while let
the glyph pass harmlessly through the caster.  That exemption is gone -- it was
never how the game worked.

"Vanilla" now means two things.  A Hexenwail build runs our gamecode unless
you asked otherwise, so it will differ from Raven's in ways that are not
engine bugs.  If you report a bug, paste the Gamecode: line, or say whether
-vanillaprogs changes what you see -- that is the fastest way to tell whether
a problem is ours or Raven's, and it deletes nothing.
EOF

            # The same three files again, this time where the engine finds them
            # by itself.  uhexen2-xsmc.
            #
            # The binary sits at <platform>/bin/glhexen2, so the engine's
            # <exedir>/../share/hexenwail/ lookup resolves to
            # <platform>/share/hexenwail/ -- which is already exactly the
            # layout packages.gamecode installs into, so nothing has to be
            # rearranged to satisfy it.  docs/BUNDLED_GAMECODE.md.
            #
            # Windows gets no equivalent because it needs none: release.yml
            # flattens that zip to a Hexen II-style root, which leaves
            # gamecode/ beside glh2.exe and satisfies the engine's first
            # lookup layer, <exedir>/gamecode/, as shipped.
            #
            # An addition, not a move: the staging tree above stays, as the
            # hand-install source gamecode/README.txt points at for feeding a
            # different engine.  (It is NOT what -vanillaprogs falls back to --
            # that switch declines the bundle and takes the player's own
            # install's gamecode.)  ~2.9 MB for the one platform dir.
            # hw/ and siege/ are withheld here for the same reason as above.
            install -Dm644 \
              ${self.packages.${system}.gamecode}/share/hexenwail/data1/progs.dat \
              ${self.packages.${system}.gamecode}/share/hexenwail/data1/progs2.dat \
              -t $out/release/linux-x86_64/share/hexenwail/data1
            install -Dm644 \
              ${self.packages.${system}.gamecode}/share/hexenwail/portals/progs.dat \
              -t $out/release/linux-x86_64/share/hexenwail/portals

            # License files
            mkdir -p $out/release/licenses
            cp ${self}/COPYING $out/release/licenses/COPYING.GPL2 2>/dev/null || \
              echo "GNU General Public License v2.0 or later — see https://www.gnu.org/licenses/gpl-2.0.html" > $out/release/licenses/COPYING.GPL2
            cp ${self}/oslibs/windows/SDL3/LICENSE.txt $out/release/licenses/LICENSE.SDL3
            cp ${self}/oslibs/windows/codecs/COPYING.ogg-vorbis $out/release/licenses/COPYING.ogg-vorbis
            # Dr. MinGW ships in both Windows bundles, so its terms travel too.
            cp ${drmingw}/licenses/* $out/release/licenses/

            # Create a release info file
            cat > $out/release/BUILD_INFO.txt <<EOF
Hexenwail Release Build
Version: ${version}
Built: $(date -u +"%Y-%m-%d %H:%M:%S UTC")

Included platforms:
- linux-x86_64/          Linux 64-bit (portable, any distro)
- windows-x86_64/        Windows 64-bit

On NixOS, install from the flake rather than from this zip:
  nix run github:bobberb/hexenwail
That builds against your own nixpkgs, so the binary's rpaths point at store
paths you actually have.  A prebuilt Nix-store binary could not.

No game data is included -- Hexenwail is an engine and ships no Raven content.
Put a "data1" directory (pak0.pak, pak1.pak) from a GOG, Steam or disc copy of
Hexen II beside the executable.  If you don't own it, each platform directory
carries a helper that downloads the free 1997 three-level demo and verifies it:

- linux-x86_64/get_demo.sh          run it from that directory
- windows-x86_64/get_demo.cmd       double-click, or run from cmd

The demo data comes from the uHexen2 project and is not ours to relicense.

Compiled gamecode (gamecode/):
This release also carries the Hexen II game logic rebuilt from source, with
fixes that are not in Raven's 1997 progs.dat -- chiefly dropped backpacks
silently vanishing in co-op and deathmatch.  The engine already loads it from
beside the executable on both platforms, so there is nothing to install and
nothing of yours is overwritten.  Launch with -vanillaprogs to ignore it and
use your own install's gamecode instead; the "Gamecode:" line the engine
prints on load always names the file it actually used.

gamecode/ is the same three files kept separately.  On Windows it is the copy
the engine loads, since it sits beside glh2.exe; on Linux the engine loads its
own from the platform directory and this is a spare.  Copying it into a Hexen
II folder is needed only to feed a DIFFERENT engine, and that overwrites files
retail keeps loose with no .pak copy to fall back on -- back up first.  Read
gamecode/README.txt; it covers all of this, and lists one deliberate behaviour
change to the Crusader's Glyph of the Ancients.

Crash reporting (Windows):
The Windows build carries Dr. MinGW, so a crash writes "glh2.RPT" beside
glh2.exe by itself -- nothing to install, nothing to turn on.  Send that file
and qconsole.log with any crash report; between them they name the source file
and line the game died on.

The .exe ships with its debug symbols, which is why those reports are readable.
It costs about 4 MB of download and nothing else: the machine code is identical
to a stripped build and the symbols are never loaded while you play.

Licenses:
- licenses/COPYING.GPL2          Engine (GPL-2.0+)
- licenses/LICENSE.SDL3           SDL3 (Zlib)
- licenses/COPYING.ogg-vorbis    libogg/libvorbis (BSD-3)
- licenses/LICENSE.drmingw       Dr. MinGW crash handler (LGPL-2.1+)
- licenses/LICENSE.libdwarf      libdwarf, via Dr. MinGW (LGPL-2.1+)
- licenses/LICENSE.zlib          zlib, via Dr. MinGW (Zlib)
- dr_mp3, dr_flac, dr_wav are public domain (dr_libs by David Reid)

Built with Nix flakes
EOF

            echo "Release bundle created in $out/release"
          '';
        };

        # Development shell for building and testing
        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            sdl3
            libGL

            libvorbis
            libogg

            alsa-lib
            fluidsynth
            libsndfile
            flac
            libxmp
            opusfile
            soundfont-fluid
            pkg-config
            gcc
            gnumake
            cmake
          ];

          shellHook = ''
            echo "Hexenwail development environment"
            echo ""
            echo "Quick commands (see: make help):"
            echo "  make nix-build      - Build Linux with Nix"
            echo "  make nix-release    - Build all platforms (Linux, Win64)"
            echo "  make build          - Build Linux with CMake"
            echo "  make release        - Build all platforms with CMake"
            echo ""
            echo "Direct Nix commands:"
            echo "  nix build .#nixos     - Linux build (NixOS), engine only"
            echo "  nix build .#nixos-bundled  - engine + our gamecode (the default)"
            echo "  nix build .#linux-fhs - Linux build (standard FHS)"
            echo "  nix build .#h2ded     - Dedicated server (headless), engine only"
            echo "  nix build .#h2ded-bundled  - dedicated server + our gamecode"
            echo "  nix build .#utils     - Map/model toolchain (qbsp, light, vis, hcc...)"
            echo "  nix build .#win64     - Windows 64-bit"
            echo "  nix build .#release   - All platforms"
            echo ""
            echo "Direct CMake commands:"
            echo "  cd engine && mkdir -p build && cd build"
            echo "  cmake .. && make"
            echo ""
            echo "Release script:"
            echo "  ./build-release.sh [nix|cmake]"
          '';
        };

        # App for easy running.  nixos-bundled, not nixos: `nix run` should
        # get our gamecode, not silently fall back to the player's retail
        # 1997 progs.dat.  uhexen2-9die.
        apps.default = {
          type = "app";
          program = "${self.packages.${system}.nixos-bundled}/bin/glhexen2";
        };

        # nix run .#get-demo [destination]
        #
        # Fetches Raven's Nov 1997 Hexen II demo data from the uHexen2 project
        # so a fresh checkout has something to run.  This is the imperative
        # route: it installs data1 into a directory you name, off the store,
        # which is what you want beside an unpacked release or a non-Nix
        # build.  packages.demodata is the declarative route to the same
        # bytes; see the redistribution constraint documented there
        # (uhexen2-3vmk).  Neither one puts demo data in an artifact we
        # publish -- both only help the user download it themselves.
        apps.get-demo = {
          type = "app";
          program = "${pkgs.writeShellApplication {
            name = "hexenwail-get-demo";
            runtimeInputs = with pkgs; [ curl gnutar gzip coreutils ];
            text = builtins.readFile ./scripts/get_demo.sh;
          }}/bin/hexenwail-get-demo";
        };
      }
    );
}
