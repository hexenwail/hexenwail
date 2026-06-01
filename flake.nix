{
  description = "neojanq — Shanjaq H2 fork archive builds (pre-2024, r6303 era)";

  # SDL note: nixpkgs' SDL attr is now sdl12-compat (real SDL 1.2 was removed
  # upstream in 2024).  Era-accurate ABI is preserved — sdl12-compat exposes
  # the SDL 1.2 ABI; binaries built against it dynamically load
  # libSDL-1.2.so.0 at runtime (which is itself sdl12-compat on current
  # distros, real SDL 1.2 on older ones — either works).  The Windows binary
  # bundles the era-accurate SDL.dll from oslibs/windows/SDL/.

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      pkgsCross64 = pkgs.pkgsCross.mingwW64;

      version = "shanjaq-r6303-archive";

      commonNative = with pkgs; [ gnumake pkg-config which ];

      linuxClientDeps = with pkgs; [
        SDL              # sdl12-compat — provides SDL 1.2 ABI
        libGL libGLU
        libvorbis libogg libopus opusfile libmikmod
        flac libmad
        alsa-lib
      ] ++ (with pkgs.xorg; [
        libX11 libXxf86vm libXxf86dga libXrandr libXext libXi
      ]);

      # Shanjaq's r6303-era code predates gcc 14's promotion of implicit
      # int->pointer conversion from warning to error.  Use gcc 13 so the
      # original source compiles unmodified (byte-for-byte provenance).
      eraGcc = pkgs.gcc13;
      eraStdenv = pkgs.overrideCC pkgs.stdenv eraGcc;

      mkLinux = { pname, srcSubdir, makeTarget, binaryName, deps ? linuxClientDeps }:
        eraStdenv.mkDerivation {
          inherit pname version;
          src = self;
          nativeBuildInputs = commonNative;
          buildInputs = deps;
          dontConfigure = true;
          enableParallelBuilding = true;
          # -fcommon: Shanjaq's era predates gcc 10's -fno-common default,
          # so headers declare uninitialized globals without 'extern' and
          # rely on the linker to merge them.  Restore via NIX_CFLAGS_COMPILE
          # so it appends to (rather than replaces) the Makefile's own CFLAGS.
          NIX_CFLAGS_COMPILE = "-fcommon";
          buildPhase = ''
            runHook preBuild
            cd ${srcSubdir}
            make ${makeTarget} USE_CODEC_TIMIDITY=no -j$NIX_BUILD_CORES
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            mkdir -p $out/bin
            install -Dm755 ${binaryName} $out/bin/${binaryName}
            runHook postInstall
          '';
          dontStrip = false;
        };

      mkWin64 = { pname, srcSubdir, makeTarget, binaryName }:
        pkgs.stdenv.mkDerivation {
          inherit pname version;
          src = self;
          nativeBuildInputs = [
            pkgsCross64.stdenv.cc
            pkgs.gnumake pkgs.which pkgs.pkg-config
          ];
          dontConfigure = true;
          enableParallelBuilding = true;
          # nixpkgs mingw cross-gcc is gcc 15 which defaults to C23 (where
          # 'false' is reserved) and promotes several historically-warning
          # diagnostics to hard errors.  Era code:
          #   - uses 'false' as an enum constant → -std=gnu99
          #   - has the same uninitialized-global pattern → -fcommon
          #   - has many int↔pointer/incompatible-pointer/implicit-decl
          #     warnings that gcc 13 (Linux build) tolerates as warnings.
          # Downgrade each promoted-to-error back to a warning so the build
          # behaves like gcc 13.  No source-level changes implied.
          NIX_CFLAGS_COMPILE = "-std=gnu99 -fcommon -Wno-error=int-conversion -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types -Wno-error=implicit-int";
          buildPhase = ''
            runHook preBuild
            export TARGET=x86_64-w64-mingw32
            export CC=$TARGET-gcc
            export AS=$TARGET-as
            export RANLIB=$TARGET-ranlib
            export AR=$TARGET-ar
            export WINDRES=$TARGET-windres
            export STRIPPER=$TARGET-strip
            export W64BUILD=1
            cd ${srcSubdir}
            make ${makeTarget} USE_CODEC_TIMIDITY=no -j$NIX_BUILD_CORES
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            mkdir -p $out/bin
            install -Dm755 ${binaryName} $out/bin/${binaryName}
            runHook postInstall
          '';
          dontStrip = true;
        };

    in
    {
      packages.${system} = {
        linux-glhexen2 = mkLinux {
          pname = "neojanq-linux-glhexen2";
          srcSubdir = "engine/hexen2";
          makeTarget = "glh2";
          binaryName = "glhexen2";   # Linux GL_BINARY; Win64 makes glh2.exe
        };
        linux-glhwcl = mkLinux {
          pname = "neojanq-linux-glhwcl";
          srcSubdir = "engine/hexenworld/client";
          makeTarget = "glhw";
          binaryName = "glhwcl";
        };
        linux-hwsv = mkLinux {
          pname = "neojanq-linux-hwsv";
          srcSubdir = "engine/hexenworld/server";
          makeTarget = "";   # default target
          binaryName = "hwsv";
          deps = [ ];        # headless server, no GL/X/SDL/audio
        };

        win64-glhexen2 = mkWin64 {
          pname = "neojanq-win64-glhexen2";
          srcSubdir = "engine/hexen2";
          makeTarget = "glh2";
          binaryName = "glh2.exe";
        };
        win64-glhwcl = mkWin64 {
          pname = "neojanq-win64-glhwcl";
          srcSubdir = "engine/hexenworld/client";
          makeTarget = "glhw";
          binaryName = "glhwcl.exe";
        };
        win64-hwsv = mkWin64 {
          pname = "neojanq-win64-hwsv";
          srcSubdir = "engine/hexenworld/server";
          makeTarget = "";
          binaryName = "hwsv.exe";
        };

        default = self.packages.${system}.release;

        # Release package: produces two zips ready for GitHub release upload.
        # linux-x86_64.zip: glhexen2, hwsv  (Linux dynamic loader resolves SDL/codecs from system)
        # windows-x86_64.zip: glh2.exe, hwsv.exe, SDL.dll, codec DLLs
        # glhwcl/glhwcl.exe are NOT included — the HW client at this Shanjaq
        # tip needs substantial source porting to compile against modern
        # toolchains and the evolved h2shared headers (tracked separately).
        release = pkgs.stdenv.mkDerivation {
          pname = "neojanq-release";
          inherit version;
          dontUnpack = true;
          nativeBuildInputs = [ pkgs.zip ];
          buildPhase = ''
            mkdir -p linux-x86_64 windows-x86_64
            cp ${self.packages.${system}.linux-glhexen2}/bin/glhexen2 linux-x86_64/
            cp ${self.packages.${system}.linux-hwsv}/bin/hwsv         linux-x86_64/
            chmod +w linux-x86_64/*

            cp ${self.packages.${system}.win64-glhexen2}/bin/glh2.exe windows-x86_64/
            cp ${self.packages.${system}.win64-hwsv}/bin/hwsv.exe     windows-x86_64/

            # Era-accurate SDL.dll (bundled in oslibs)
            cp ${self}/oslibs/windows/SDL/lib64/SDL.dll               windows-x86_64/
            # Codec DLLs that glh2.exe imports
            cp ${self}/oslibs/windows/codecs/x64/libFLAC-8.dll        windows-x86_64/
            cp ${self}/oslibs/windows/codecs/x64/libmad-0.dll         windows-x86_64/
            cp ${self}/oslibs/windows/codecs/x64/libmikmod-3.dll      windows-x86_64/
            cp ${self}/oslibs/windows/codecs/x64/libvorbisfile-3.dll  windows-x86_64/
            cp ${self}/oslibs/windows/codecs/x64/libvorbis-0.dll      windows-x86_64/
            cp ${self}/oslibs/windows/codecs/x64/libogg-0.dll         windows-x86_64/
            cp ${self}/oslibs/windows/codecs/x64/libopusfile-0.dll    windows-x86_64/
            cp ${self}/oslibs/windows/codecs/x64/libopus-0.dll        windows-x86_64/
            chmod +w windows-x86_64/*

            cat > README.txt <<'EOF'
            Shanjaq H2 fork archive builds (pre-2024 / SVN r6303-era).
            Anchored at commit 800586575 of github.com/shanjaq/uhexen2 (2021-12-06).

            Built from the unmodified Shanjaq source (a small number of
            build-infrastructure patches were required for modern Linux
            toolchains and the Win64 cross — see commit log on the
            shanjaq-r6303-builds branch).

            Linux x86_64:
              glhexen2  — single-player H2 GL client
              hwsv      — HexenWorld dedicated server
              Requires: libSDL-1.2.so.0 (or sdl12-compat), libGL, libvorbis,
                        libogg, libopus, opusfile, libmikmod, libFLAC, libmad,
                        alsa-lib at runtime (typical distro packages).

            Windows x86_64:
              glh2.exe  — single-player H2 GL client
              hwsv.exe  — HexenWorld dedicated server
              SDL.dll + codec DLLs bundled in this directory; keep them
              alongside the .exe.

            NOT INCLUDED: glhwcl / glhwcl.exe (HexenWorld GL client).  The
            HW client at this Shanjaq tip has structural divergence from
            the evolved h2shared headers and needs substantial source
            porting to build on modern toolchains.
            EOF
            cp README.txt linux-x86_64/
            cp README.txt windows-x86_64/

            zip -r9 shanjaq-r6303-archive-linux-x86_64.zip   linux-x86_64
            zip -r9 shanjaq-r6303-archive-windows-x86_64.zip windows-x86_64
          '';
          installPhase = ''
            mkdir -p $out
            cp shanjaq-r6303-archive-linux-x86_64.zip   $out/
            cp shanjaq-r6303-archive-windows-x86_64.zip $out/
          '';
        };
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = commonNative ++ linuxClientDeps ++ [
          pkgsCross64.stdenv.cc pkgs.gcc
        ];
        shellHook = ''
          echo "neojanq build shell — Shanjaq r6303-era builds"
          echo "Linux native: cd engine/hexen2 && make glh2"
          echo "Win64 cross:  cd engine/hexen2 && W64BUILD=1 CC=x86_64-w64-mingw32-gcc make glh2"
        '';
      };
    };
}
