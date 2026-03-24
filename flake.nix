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
        pkgs = import nixpkgs { inherit system; };
        pkgsCross64 = import nixpkgs {
          inherit system;
          crossSystem = {
            config = "x86_64-w64-mingw32";
          };
        };

        version = "1.5.10-unstable-${self.lastModifiedDate}";

      in
      {
        packages = let
          # Shared build configuration for Linux builds
          linuxBuildAttrs = {
            pname = "glhexen2";
            inherit version;

            src = ./.;

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
              "-DSOUNDFONT_PATH=${pkgs.soundfont-fluid}/share/soundfonts/FluidR3_GM2-2.sf3"
            ];

            meta = with pkgs.lib; {
              description = "Hammer of Thyrion - Hexen II source port (OpenGL version)";
              longDescription = ''
                Hexen II: Hammer of Thyrion (uHexen2) is a cross-platform port of
                Raven Software's Hexen II source. It is based on an older linux port,
                Anvil of Thyrion.

                Note: This package only provides the game engine. You need the original
                game data files (pak0.pak, pak1.pak) from the commercial game to play.
              '';
              homepage = "https://github.com/bobberb/hexenwail";
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
              mkdir -p $out/share/uhexen2

              # Install the OpenGL binary from CMake build directory
              install -Dm755 bin/glhexen2 $out/bin/glhexen2


              runHook postInstall
            '';
          });

          default = self.packages.${system}.nixos;

          # OpenGL version for standard FHS Linux systems (non-NixOS)
          # Repackages the nixos build with FHS paths instead of recompiling
          linux-fhs = pkgs.runCommand "glhexen2-linux-fhs-${self.packages.${system}.nixos.version}" {
            nativeBuildInputs = [ pkgs.patchelf ];
          } ''
            mkdir -p $out/bin
            cp ${self.packages.${system}.nixos}/bin/glhexen2 $out/bin/glhexen2
            chmod +w $out/bin/glhexen2
            patchelf --set-interpreter /lib64/ld-linux-x86-64.so.2 --remove-rpath $out/bin/glhexen2
          '';

          # Windows 64-bit build
          win64 = pkgsCross64.stdenv.mkDerivation {
            pname = "glhexen2-win64";
            inherit version;

            src = ./.;

            nativeBuildInputs = with pkgs; [
              cmake
              pkg-config
            ];

            buildInputs = with pkgsCross64; [
              windows.pthreads
            ];

            # CMake is in engine subdirectory
            preConfigure = ''
              cd engine
            '';

            cmakeFlags = [
              "-DUSE_CODEC_VORBIS=ON"
              "-DUSE_CODEC_OPUS=OFF"  # opusfile cross-compile not supported via nix
              "-DUSE_CODEC_XMP=OFF"   # libxmp has a nixpkgs cycle bug in mingw cross-build
            ];

            installPhase = ''
              runHook preInstall

              mkdir -p $out/bin

              # Install the Windows executable
              install -Dm755 bin/glh2.exe $out/bin/glh2.exe

              # Install DLLs from build output (MinGW runtime)
              for dll in bin/*.dll; do
                [ -f "$dll" ] && install -Dm755 "$dll" $out/bin/
              done

              # Bundle SDL3
              install -Dm755 ../../oslibs/windows/SDL3/lib64/SDL3.dll $out/bin/SDL3.dll

              # Bundle OGG/Vorbis codec DLLs
              for dll in ../../oslibs/windows/codecs/x64/*.dll; do
                install -Dm755 "$dll" $out/bin/
              done

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
              $STRIP -S -p $out/bin/*.exe $out/bin/*.dll 2>/dev/null || true
            '';

            meta = with pkgs.lib; {
              description = "Hammer of Thyrion - Hexen II source port (OpenGL, Windows 64-bit)";
              homepage = "https://github.com/bobberb/hexenwail";
              license = licenses.gpl2Plus;
              platforms = platforms.windows;
              maintainers = [ ];
            };
          };

          # Release package - builds all platforms together
          release = pkgs.runCommand "glhexen2-release-${version}" {
            meta = with pkgs.lib; {
              description = "Hammer of Thyrion - Multi-platform release bundle";
              homepage = "https://github.com/bobberb/hexenwail";
              license = licenses.gpl2Plus;
              platforms = platforms.linux;
            };
          } ''
            mkdir -p $out/release

            # Linux portable (FHS binary, runs on any distro)
            mkdir -p $out/release/linux-x86_64
            cp -r ${self.packages.${system}.linux-fhs}/bin $out/release/linux-x86_64/

            # Linux NixOS (nix store rpaths)
            mkdir -p $out/release/linux-x86_64-nixos
            cp -r ${self.packages.${system}.nixos}/bin $out/release/linux-x86_64-nixos/

            # Windows 64-bit (dereference symlinks so DLLs are real files)
            mkdir -p $out/release/windows-x86_64
            cp -rL ${self.packages.${system}.win64}/bin $out/release/windows-x86_64/

            # License files
            mkdir -p $out/release/licenses
            cp ${self}/COPYING $out/release/licenses/COPYING.GPL2 2>/dev/null || \
              echo "GNU General Public License v2.0 or later — see https://www.gnu.org/licenses/gpl-2.0.html" > $out/release/licenses/COPYING.GPL2
            cp ${self}/oslibs/windows/SDL3/LICENSE.txt $out/release/licenses/LICENSE.SDL3
            cp ${self}/oslibs/windows/codecs/COPYING.ogg-vorbis $out/release/licenses/COPYING.ogg-vorbis

            # Create a release info file
            cat > $out/release/BUILD_INFO.txt <<EOF
Hexenwail Release Build
Version: ${version}
Built: $(date -u +"%Y-%m-%d %H:%M:%S UTC")

Included platforms:
- linux-x86_64/          Linux 64-bit (portable, any distro)
- linux-x86_64-nixos/    Linux 64-bit (NixOS)
- windows-x86_64/        Windows 64-bit

Licenses:
- licenses/COPYING.GPL2          Engine (GPL-2.0+)
- licenses/LICENSE.SDL3           SDL3 (Zlib)
- licenses/COPYING.ogg-vorbis    libogg/libvorbis (BSD-3)
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
            echo "  nix build .#nixos     - Linux build (NixOS)"
            echo "  nix build .#linux-fhs - Linux build (standard FHS)"
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

        # App for easy running
        apps.default = {
          type = "app";
          program = "${self.packages.${system}.nixos}/bin/glhexen2";
        };
      }
    );
}
