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
        flac libmad libtimidity
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
          buildPhase = ''
            runHook preBuild
            cd ${srcSubdir}
            make ${makeTarget} -j$NIX_BUILD_CORES
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
            make ${makeTarget} -j$NIX_BUILD_CORES
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
          binaryName = "glh2";
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

        default = self.packages.${system}.linux-glhexen2;
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
