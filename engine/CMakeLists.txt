# CMake build for Hexen II OpenGL client (glhexen2)
# Builds for: Linux (x86_64) and Windows 64-bit

cmake_minimum_required(VERSION 3.16)
project(glhexen2 VERSION 1.5.10 LANGUAGES C)

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Project paths
set(ENGINE_TOP ${CMAKE_CURRENT_SOURCE_DIR})
set(COMMONDIR ${ENGINE_TOP}/h2shared)
set(UHEXEN2_TOP ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(UHEXEN2_SHARED ${UHEXEN2_TOP}/common)
set(LIBS_DIR ${UHEXEN2_TOP}/libs)
set(OSLIBS ${UHEXEN2_TOP}/oslibs)

#============================================================================
# Build Options
#============================================================================
option(USE_DEBUG "Debug build" OFF)
option(USE_DEBUGINFO "Release build, but keep DWARF so crashes can be backtraced" OFF)
option(USE_SOUND "Enable sound support" ON)
option(USE_ALSA "Enable ALSA audio (Linux)" ON)
option(USE_CODEC_VORBIS "Enable Ogg Vorbis codec" ON)
option(USE_CODEC_MP3 "Enable MP3 codec (dr_mp3)" ON)
option(USE_CODEC_FLAC "Enable FLAC codec" ON)
option(USE_CODEC_XMP "Enable tracker music via libxmp" ON)
option(USE_CODEC_UMX "Enable Unreal UMX container support" ON)
option(USE_CODEC_OPUS "Enable Opus codec" ON)
option(USE_FLUIDSYNTH "Use FluidSynth as the MIDI driver when available" ON)
# BGM tier 2 only: activated solely on targets where no MIDI driver claims
# tier 1 (no FluidSynth, or WASM).  FluidSynth always wins where present.
option(USE_CODEC_TIMIDITY "Enable the vendored libTiMidity MIDI fallback codec" ON)

# Off by default so `nix build` keeps producing exactly the client it does
# today; the server is a separate, rarely-wanted artifact.
option(BUILD_DEDICATED "Also build the h2ded dedicated server" OFF)

# The GL ES 3.0 render tier.  Emscripten/WebGL2 is one *instance* of it, not
# the definition of it: everything the ES path does differently -- statically
# linked entry points instead of dlsym, no SSBO/compute, no buffer_storage or
# fence sync, "#version 300 es" shader headers, an ES context request -- follows
# from the API level, not from running in a browser.  Keying those on
# __EMSCRIPTEN__ made the browser the only way to reach the path.  Forced ON for
# Emscripten below, and settable on a desktop build (-DUSE_GLES=ON) to exercise
# the same tier against Mesa, which is also the cheap way to validate the ANGLE
# route to macOS/Metal without a Mac.  uhexen2-0py6.
option(USE_GLES "Target the OpenGL ES 3.0 render path instead of desktop GL 4.3" OFF)

# Bundled gamecode (docs/BUNDLED_GAMECODE.md).  Third and last lookup layer,
# for distribution packagers who install the data somewhere neither
# <exedir>/gamecode/ nor <exedir>/../share/hexenwail/ finds.  Unset by default:
# the two exedir-relative layers cover the release zips unaided.  Applied with
# add_definitions() rather than target_compile_definitions() because both the
# client and h2ded compile the substitution.
if(BUNDLED_GAMECODE_DIR)
    add_definitions(-DBUNDLED_GAMECODE_DIR="${BUNDLED_GAMECODE_DIR}")
    message(STATUS "Bundled gamecode fallback path: ${BUNDLED_GAMECODE_DIR}")
endif()

#============================================================================
# Platform Detection
#============================================================================
if(EMSCRIPTEN)
    set(TARGET_OS "web")
elseif(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(TARGET_OS "win64")
        set(ARCH_DIR "x64")
    else()
        set(TARGET_OS "win32")
        set(ARCH_DIR "x86")
    endif()
elseif(UNIX)
    set(TARGET_OS "unix")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

#============================================================================
# Git Version
#============================================================================
# Append git describe suffix for dev builds (e.g. "26.03-alpha.7k-3-gabcdef")
# Tagged commits get the exact tag; the hardcoded HW_VERSION is the fallback.
execute_process(
    COMMAND git describe --tags --always
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RESULT
)
if(GIT_RESULT EQUAL 0 AND GIT_VERSION)
    add_definitions(-DHW_GIT_VERSION="${GIT_VERSION}")
    message(STATUS "Git version: ${GIT_VERSION}")
endif()

#============================================================================
# Compiler Flags
#============================================================================
# Common flags
add_compile_options(-Wall -Wno-error=unused-result -Wno-error=array-bounds)

if(USE_DEBUG)
    add_compile_options(-g -DDEBUG=1 -DDEBUG_BUILD=1)
elseif(USE_DEBUGINFO)
    # A shipping build that can still be backtraced: byte-for-byte the same
    # flags as the plain release below, plus -g.  -g does not affect codegen,
    # so this produces the identical .text -- it only adds DWARF sections the
    # loader never maps, which is what lets us ship symbols to players at no
    # runtime cost and get a real stack trace back when they crash.
    #
    # Note -fomit-frame-pointer stays.  Forcing frame pointers was tried and
    # dropped: on x86-64 Windows, unwinding goes through the mandatory
    # .pdata/.xdata tables rather than RBP, and a test binary built both ways
    # produced the same resolved backtrace.  uhexen2-hger.
    add_compile_options(-O2 -DNDEBUG=1 -ffast-math -fomit-frame-pointer -g)
else()
    add_compile_options(-O2 -DNDEBUG=1 -ffast-math -fomit-frame-pointer)
endif()

# Platform-specific defines.
# GLQUAKE/SDLQUAKE/GL_DLSYM/NO_SPLASHES select the renderer, video backend and
# splash behaviour, so they belong to the client alone -- h2ded must never see
# them (quakeinc.h keys its whole header set off SERVERONLY vs GLQUAKE).  They
# are collected here and applied with target_compile_definitions() once the
# client target exists rather than via add_definitions(), which would leak them
# into every target in this directory.
if(EMSCRIPTEN)
    add_definitions(-D__EMSCRIPTEN__)
    # The browser has no GL to dlsym: WebGL2 entry points are linked in
    # statically, which is exactly what USE_GLES selects below.
    set(USE_GLES ON)
    set(CLIENT_DEFINITIONS GLQUAKE SDLQUAKE NO_SPLASHES)
    set(USE_ALSA OFF)
    set(USE_CODEC_FLAC OFF)
    set(USE_CODEC_OPUS OFF)
    set(USE_CODEC_XMP OFF)
elseif(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    set(CLIENT_DEFINITIONS GLQUAKE SDLQUAKE GL_DLSYM NO_SPLASHES)
    set(USE_ALSA OFF)
elseif(UNIX)
    add_definitions(-D_GNU_SOURCE=1 -D_REENTRANT)
    set(CLIENT_DEFINITIONS GLQUAKE SDLQUAKE GL_DLSYM)
endif()

# GL_DLSYM and USE_GLES are mutually exclusive: the ES tier resolves its entry
# points at link time (gl_func.h maps every _fp name straight onto the real
# function), so there is nothing for the dlsym loader to fill in and its
# GL_FUNCTION table would declare pointers that the ES branch never emits.
# Dropping it here is what makes a desktop -DUSE_GLES=ON build take the same
# code path Emscripten already takes, rather than a third hybrid one.
if(USE_GLES AND "GL_DLSYM" IN_LIST CLIENT_DEFINITIONS)
    list(REMOVE_ITEM CLIENT_DEFINITIONS GL_DLSYM)
endif()

# Applied with add_definitions() rather than target_compile_definitions()
# because the ES tier reaches into h2shared sources that both the client and
# any future GL-linked target compile, and glheader.h/gl_func.h are included
# from files configured outside CLIENT_DEFINITIONS' target.
if(USE_GLES)
    add_definitions(-DUSE_GLES=1)
    message(STATUS "Render tier: OpenGL ES 3.0 (USE_GLES)")
endif()

#============================================================================
# Include Directories
#============================================================================
include_directories(
    ${ENGINE_TOP}/hexen2
    ${COMMONDIR}
    ${UHEXEN2_SHARED}
)

# Windows-specific includes
if(WIN32)
    include_directories(${OSLIBS}/windows/misc/include)
endif()

#============================================================================
# Find Dependencies
#============================================================================
# SDL3 is required
if(WIN32)
    # Use prebuilt SDL3 from oslibs for Windows
    set(SDL_INCLUDE_DIR ${OSLIBS}/windows/SDL3/include)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(SDL_LIBRARY
            ${OSLIBS}/windows/SDL3/lib64/libSDL3.dll.a
        )
    else()
        set(SDL_LIBRARY
            ${OSLIBS}/windows/SDL3/lib/libSDL3.dll.a
        )
    endif()
    set(SDL_FOUND TRUE)
else()
    if(EMSCRIPTEN)
        # Emscripten: find SDL3 via both find_package and pkg-config
        set(SDL_FOUND TRUE)  # Assume SDL3 is available
        find_package(SDL3 QUIET)
        if(SDL3_FOUND)
            set(SDL_INCLUDE_DIR ${SDL3_INCLUDE_DIRS})
        else()
            # Try pkg-config as fallback
            find_package(PkgConfig QUIET)
            if(PKG_CONFIG_FOUND)
                pkg_check_modules(SDL3 QUIET sdl3)
                if(SDL3_FOUND)
                    set(SDL_INCLUDE_DIR ${SDL3_INCLUDE_DIRS})
                else()
                    message(WARNING "SDL3 not found via pkg-config, trying system paths")
                    set(SDL_INCLUDE_DIR "/usr/include")
                endif()
            else()
                message(WARNING "pkg-config not found, using system paths for SDL3")
                set(SDL_INCLUDE_DIR "/usr/include")
            endif()
        endif()
        set(SDL_LIBRARY "")  # Link flag is set in target_link_options (-sUSE_SDL=3)
    else()
        find_package(SDL3 QUIET)
        if(SDL3_FOUND)
            set(SDL_INCLUDE_DIR ${SDL3_INCLUDE_DIRS})
            set(SDL_LIBRARY SDL3::SDL3)
        else()
            # Fallback to pkg-config
            find_package(PkgConfig REQUIRED)
            pkg_check_modules(SDL3 REQUIRED sdl3)
            set(SDL_INCLUDE_DIR ${SDL3_INCLUDE_DIRS})
            set(SDL_LIBRARY ${SDL3_LIBRARIES})
        endif()
    endif()
endif()

if(NOT EMSCRIPTEN)
    if(UNIX)
        # Find packages using pkg-config.  Hoisted above the GL lookup because
        # the ES tier is discovered through it (there is no find_package(GLES)).
        find_package(PkgConfig QUIET)
        if(NOT PKG_CONFIG_FOUND)
            message(FATAL_ERROR "pkg-config is required for Linux builds")
        endif()

        # OpenGL, or the ES 3.0 tier when USE_GLES is on.  libglvnd supplies
        # both glesv2.pc and the GLES3/ headers glheader.h reaches for, so a
        # desktop ES build needs no extra dependency beyond the GL package
        # already present.
        if(USE_GLES)
            pkg_check_modules(GLESV2 REQUIRED glesv2)
        else()
            find_package(OpenGL REQUIRED)
        endif()

        # ALSA
        if(USE_ALSA)
            pkg_check_modules(ALSA REQUIRED alsa)
            set(USE_ALSA_FOUND ON)
        endif()

        # MP3: uses built-in dr_mp3 (no external library needed)
        # FLAC: uses built-in dr_flac (no external library needed)

        # m and dl
        find_library(M_LIBRARY m)
        find_library(DL_LIBRARY dl)
    endif()
endif()


# Codec discovery via pkg-config (skip on Emscripten)
if(NOT EMSCRIPTEN)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        if(USE_CODEC_VORBIS)
            pkg_check_modules(VORBIS vorbisfile vorbis ogg)
        endif()
        if(USE_CODEC_XMP)
            pkg_check_modules(XMP libxmp)
        endif()
        if(USE_CODEC_OPUS)
            pkg_check_modules(OPUS opusfile)
        endif()
    endif()
endif()

# Degrade instead of failing when an optional codec's dev files are absent.
# The USE_CODEC_* options default to ON and were used directly to add the
# source file and -DUSE_CODEC_x, while only the *link* step was guarded by
# the pkg-config result — so a missing libvorbis/libxmp/opusfile dev package
# broke the build (missing headers, or an unresolved symbol at link) instead
# of simply dropping that codec.  Only auto-disable when pkg-config actually
# ran and reported the module missing; if pkg-config itself is unavailable we
# cannot conclude anything and leave the user's choice alone.
if(NOT EMSCRIPTEN AND PKG_CONFIG_FOUND)
    # Vorbis is exempt on Windows: the oslibs prebuilt fallback just below
    # supplies it without pkg-config.
    if(USE_CODEC_VORBIS AND NOT VORBIS_FOUND AND NOT WIN32)
        message(STATUS "Vorbis dev files not found - disabling USE_CODEC_VORBIS")
        set(USE_CODEC_VORBIS OFF)
    endif()
    if(USE_CODEC_XMP AND NOT XMP_FOUND)
        message(STATUS "libxmp not found - disabling USE_CODEC_XMP")
        set(USE_CODEC_XMP OFF)
    endif()
    if(USE_CODEC_OPUS AND NOT OPUS_FOUND)
        message(STATUS "opusfile not found - disabling USE_CODEC_OPUS")
        set(USE_CODEC_OPUS OFF)
    endif()
endif()
# MP3/FLAC/UMX are deliberately not listed: dr_mp3/dr_flac are header-only and
# UMX is built in, so they have no external dev files that can go missing.

if(WIN32 AND NOT VORBIS_FOUND)
    # Fallback: use prebuilt codecs from oslibs for Windows
    set(CODEC_INCLUDE_DIR ${OSLIBS}/windows/codecs/include)
    set(CODEC_LIB_DIR ${OSLIBS}/windows/codecs/${ARCH_DIR})
    include_directories(${CODEC_INCLUDE_DIR})
    link_directories(${CODEC_LIB_DIR})
endif()

#============================================================================
# Source Files
#============================================================================

# GL Renderer sources
set(GL_RENDERER_SOURCES
    # From hexen2 (client-specific GL)
    ${ENGINE_TOP}/hexen2/gl_rmain.c
    ${ENGINE_TOP}/hexen2/gl_rmisc.c
    ${ENGINE_TOP}/hexen2/gl_rsurf.c
    ${ENGINE_TOP}/hexen2/r_part.c
    # From h2shared
    ${COMMONDIR}/gl_refrag.c
    ${COMMONDIR}/gl_rlight.c
    ${COMMONDIR}/gl_sky.c
    ${COMMONDIR}/gl_screen.c
    ${COMMONDIR}/gl_warp.c
    ${COMMONDIR}/gl_draw.c
    ${COMMONDIR}/gl_fog.c
    ${COMMONDIR}/gl_mesh.c
    ${COMMONDIR}/gl_model.c
    ${COMMONDIR}/md5mesh.c
    ${COMMONDIR}/gl_texmgr.c
    ${COMMONDIR}/gl_postprocess.c
    ${COMMONDIR}/gl_worldcull.c
    ${COMMONDIR}/gl_matrix.c
    ${COMMONDIR}/gl_shader.c
    ${COMMONDIR}/gl_buffer.c
    ${COMMONDIR}/gl_vbo.c
    ${COMMONDIR}/image.c
    ${COMMONDIR}/img_load.c
    ${COMMONDIR}/img_dds.c
)

# Client sources from hexen2
set(CLIENT_SOURCES
    ${ENGINE_TOP}/hexen2/cl_demo.c
    ${ENGINE_TOP}/hexen2/cl_effect.c
    ${ENGINE_TOP}/hexen2/cl_inlude.c
    ${ENGINE_TOP}/hexen2/cl_input.c
    ${ENGINE_TOP}/hexen2/cl_main.c
    ${ENGINE_TOP}/hexen2/cl_parse.c
    ${ENGINE_TOP}/hexen2/cl_string.c
    ${ENGINE_TOP}/hexen2/cl_tent.c
    ${ENGINE_TOP}/hexen2/cl_cmd.c
    ${ENGINE_TOP}/hexen2/console.c
    ${ENGINE_TOP}/hexen2/keys.c
    ${ENGINE_TOP}/hexen2/menu.c
    ${ENGINE_TOP}/hexen2/sbar.c
    ${ENGINE_TOP}/hexen2/view.c
    ${ENGINE_TOP}/hexen2/chase.c
    ${ENGINE_TOP}/hexen2/cl_csqc.c
    ${COMMONDIR}/pr_csqc.c
)

# Server and common sources
set(COMMON_SOURCES
    # From hexen2
    ${ENGINE_TOP}/hexen2/host.c
    ${ENGINE_TOP}/hexen2/host_cmd.c
    ${ENGINE_TOP}/hexen2/host_async.c
    ${ENGINE_TOP}/hexen2/sv_main.c
    ${ENGINE_TOP}/hexen2/sv_phys.c
    ${ENGINE_TOP}/hexen2/sv_user.c
    ${ENGINE_TOP}/hexen2/sv_effect.c
    ${ENGINE_TOP}/hexen2/sv_move.c
    ${ENGINE_TOP}/hexen2/world.c
    ${ENGINE_TOP}/hexen2/net_main.c
    ${ENGINE_TOP}/hexen2/net_loop.c
    ${ENGINE_TOP}/hexen2/net_dgrm.c
    # From h2shared
    ${COMMONDIR}/cmd.c
    ${COMMONDIR}/cvar.c
    ${COMMONDIR}/pr_cmds.c
    ${COMMONDIR}/pr_edict.c
    ${COMMONDIR}/pr_exec.c
    ${COMMONDIR}/mathlib.c
    ${COMMONDIR}/sizebuf.c
    ${COMMONDIR}/link_ops.c
    ${COMMONDIR}/hashindex.c
    ${COMMONDIR}/zone.c
    ${COMMONDIR}/quakefs.c
    ${COMMONDIR}/debuglog.c
    ${COMMONDIR}/host_string.c
    ${COMMONDIR}/cfgfile.c
    ${COMMONDIR}/common.c
    ${COMMONDIR}/wad.c
    ${COMMONDIR}/msg_io.c
    ${COMMONDIR}/sys_exedir.c
    # From common directory
    ${UHEXEN2_SHARED}/qsnprint.c
    ${UHEXEN2_SHARED}/strlcat.c
    ${UHEXEN2_SHARED}/strlcpy.c
    ${UHEXEN2_SHARED}/q_endian.c
    ${UHEXEN2_SHARED}/crc.c
)

#============================================================================
# Sound Sources
#============================================================================
if(USE_SOUND)
    set(SOUND_SOURCES
        ${COMMONDIR}/snd_sys.c
        ${COMMONDIR}/snd_dma.c
        ${COMMONDIR}/snd_mix.c
        ${COMMONDIR}/snd_mem.c
        ${COMMONDIR}/snd_wave.c
        ${COMMONDIR}/snd_mp3tag.c
        ${COMMONDIR}/bgmusic.c
        ${COMMONDIR}/snd_codec.c
    )

    # Optional codecs
    if(USE_CODEC_VORBIS)
        list(APPEND SOUND_SOURCES ${COMMONDIR}/snd_vorbis.c)
        add_definitions(-DUSE_CODEC_VORBIS)
    endif()

    if(USE_CODEC_MP3)
        list(APPEND SOUND_SOURCES ${COMMONDIR}/snd_mp3.c)
        add_definitions(-DUSE_CODEC_MP3)
    endif()

    if(USE_CODEC_FLAC)
        list(APPEND SOUND_SOURCES ${COMMONDIR}/snd_flac.c)
        add_definitions(-DUSE_CODEC_FLAC)
    endif()

    if(USE_CODEC_XMP)
        list(APPEND SOUND_SOURCES ${COMMONDIR}/snd_xmp.c)
        add_definitions(-DUSE_CODEC_XMP)
    endif()

    if(USE_CODEC_UMX)
        list(APPEND SOUND_SOURCES ${COMMONDIR}/snd_umx.c)
        add_definitions(-DUSE_CODEC_UMX)
    endif()

    if(USE_CODEC_OPUS)
        list(APPEND SOUND_SOURCES ${COMMONDIR}/snd_opus.c)
        add_definitions(-DUSE_CODEC_OPUS)
    endif()

    # Platform-specific sound (SDL3 on all platforms)
    if(WIN32)
        list(APPEND SOUND_SOURCES ${COMMONDIR}/snd_sdl.c)
    elseif(UNIX)
        list(APPEND SOUND_SOURCES ${COMMONDIR}/snd_sdl.c)
        if(USE_ALSA_FOUND)
            list(APPEND SOUND_SOURCES ${COMMONDIR}/snd_alsa.c)
        else()
            add_definitions(-DNO_ALSA_AUDIO)
        endif()
    endif()
else()
    set(SOUND_SOURCES ${COMMONDIR}/snd_null.c ${COMMONDIR}/bgmnull.c)
    add_definitions(-D_NO_SOUND)
endif()

#============================================================================
# Platform-Specific Sources
#============================================================================
if(EMSCRIPTEN)
    # Video / Input / System — same SDL3 backends as Unix
    list(APPEND PLATFORM_SOURCES
        ${COMMONDIR}/gl_vidsdl.c
        ${COMMONDIR}/in_sdl.c
        ${ENGINE_TOP}/hexen2/sys_unix.c
        ${COMMONDIR}/sys_sdl.c
    )

    # Network — loopback only (no raw sockets in WASM)
    list(APPEND PLATFORM_SOURCES
        ${ENGINE_TOP}/hexen2/net_bsd.c
        ${ENGINE_TOP}/hexen2/net_udp.c
    )

    # No MIDI driver (tier 1); MIDI plays through the libTiMidity codec instead
    list(APPEND PLATFORM_SOURCES ${COMMONDIR}/midi_nul.c)
    add_definitions(-D_NO_MIDIDRV)
    set(MIDI_CODEC_FALLBACK ON)
    list(APPEND PLATFORM_SOURCES ${COMMONDIR}/cd_sdl.c)

elseif(WIN32)
    # Video (SDL3 backend, same as Unix)
    list(APPEND PLATFORM_SOURCES ${COMMONDIR}/gl_vidsdl.c)

    # Input (SDL3 backend, same as Unix)
    list(APPEND PLATFORM_SOURCES ${COMMONDIR}/in_sdl.c)

    # Network
    list(APPEND PLATFORM_SOURCES
        ${ENGINE_TOP}/hexen2/net_win.c
        ${ENGINE_TOP}/hexen2/net_wins.c
    )

    # System
    list(APPEND PLATFORM_SOURCES
        ${ENGINE_TOP}/hexen2/sys_win.c
        ${COMMONDIR}/sys_sdl.c
    )

    # MIDI (Windows native midiStream API)
    list(APPEND PLATFORM_SOURCES ${COMMONDIR}/midi_win.c ${COMMONDIR}/mid2strm.c)

    # CD Audio (SDL3 -- falls back to cd_null.c)
    list(APPEND PLATFORM_SOURCES ${COMMONDIR}/cd_sdl.c)

    # Resources
    set(RESOURCES ${COMMONDIR}/win32res.rc)

elseif(UNIX)
    # Video
    list(APPEND PLATFORM_SOURCES ${COMMONDIR}/gl_vidsdl.c)

    # Input
    list(APPEND PLATFORM_SOURCES ${COMMONDIR}/in_sdl.c)

    # Network
    list(APPEND PLATFORM_SOURCES
        ${ENGINE_TOP}/hexen2/net_bsd.c
        ${ENGINE_TOP}/hexen2/net_udp.c
    )

    # System
    list(APPEND PLATFORM_SOURCES
        ${ENGINE_TOP}/hexen2/sys_unix.c
        ${COMMONDIR}/sys_sdl.c
    )

    # MIDI (FluidSynth on Linux, with the libTiMidity codec as fallback)
    find_package(PkgConfig)
    # -DUSE_FLUIDSYNTH=OFF makes the fallback arm reachable on machines that
    # do have FluidSynth, which is the only way to exercise it in a test.
    if(PKG_CONFIG_FOUND AND USE_FLUIDSYNTH)
        pkg_check_modules(FLUIDSYNTH fluidsynth)
    endif()
    if(FLUIDSYNTH_FOUND)
        list(APPEND PLATFORM_SOURCES ${COMMONDIR}/midi_fluid.c)
        set(SOUNDFONT_SOURCES ${COMMONDIR}/soundfont.c)
        include_directories(${FLUIDSYNTH_INCLUDE_DIRS})
        if(SOUNDFONT_PATH)
            add_definitions(-DSOUNDFONT_PATH="${SOUNDFONT_PATH}")
            message(STATUS "FluidSynth MIDI: enabled (soundfont: ${SOUNDFONT_PATH})")
        else()
            message(STATUS "FluidSynth MIDI: enabled")
        endif()
    else()
        list(APPEND PLATFORM_SOURCES ${COMMONDIR}/midi_nul.c)
        add_definitions(-D_NO_MIDIDRV)
        set(MIDI_CODEC_FALLBACK ON)
        message(STATUS "FluidSynth MIDI: not found, disabled")
    endif()

    # CD Audio
    list(APPEND PLATFORM_SOURCES
        ${COMMONDIR}/cd_sdl.c
        ${COMMONDIR}/cd_bsd.c
        ${COMMONDIR}/cd_linux.c
    )
endif()

#============================================================================
# MIDI Fallback Codec (libTiMidity, vendored)
#============================================================================
# BGM runs MIDI as two tiers (see bgmusic.c): a MIDI driver first, then this
# streamer codec.  It is compiled only where tier 1 is midi_nul.c, so it can
# never displace FluidSynth or the Windows midiStream driver.  Pure C with no
# external dependencies, which is why it -- and not FluidSynth -- can go to
# WASM.  Reads the same SF2 file FluidSynth uses (libtimidity >= 0.2.8).
if(USE_SOUND AND USE_CODEC_TIMIDITY AND MIDI_CODEC_FALLBACK)
    set(TIMIDITY_DIR ${LIBS_DIR}/timidity)
    list(APPEND SOUND_SOURCES
        ${COMMONDIR}/snd_timidity.c
        ${TIMIDITY_DIR}/common.c
        ${TIMIDITY_DIR}/instrum.c
        ${TIMIDITY_DIR}/mix.c
        ${TIMIDITY_DIR}/output.c
        ${TIMIDITY_DIR}/playmidi.c
        ${TIMIDITY_DIR}/readmidi.c
        ${TIMIDITY_DIR}/readsbk.c
        ${TIMIDITY_DIR}/resample.c
        ${TIMIDITY_DIR}/sndfont.c
        ${TIMIDITY_DIR}/stream.c
        ${TIMIDITY_DIR}/tables.c
        ${TIMIDITY_DIR}/timidity.c
    )
    include_directories(${TIMIDITY_DIR})
    # Compiled straight into the executable, not linked as a DLL: keeps
    # TIMI_EXPORT empty instead of __declspec(dllimport) on Windows/OS2.
    add_definitions(-DUSE_CODEC_TIMIDITY -DTIMIDITY_BUILD=1 -DTIMIDITY_STATIC=1)
    set(SOUNDFONT_SOURCES ${COMMONDIR}/soundfont.c)
    set(TIMIDITY_CODEC_ENABLED ON)
else()
    set(TIMIDITY_CODEC_ENABLED OFF)
endif()

#============================================================================
# Create Executable
#============================================================================
# Combine all sources
set(ALL_SOURCES
    ${GL_RENDERER_SOURCES}
    ${CLIENT_SOURCES}
    ${COMMON_SOURCES}
    ${SOUND_SOURCES}
    ${SOUNDFONT_SOURCES}
    ${PLATFORM_SOURCES}
)

# Create the executable
if(EMSCRIPTEN)
    set(EXECUTABLE_NAME hexenwail)
    add_executable(${EXECUTABLE_NAME} ${ALL_SOURCES})
    # Emscripten link flags: WebGL2, memory growth, shell template
    # For pure Nix builds, use: nix develop -f shell-wasm.nix
    target_link_options(${EXECUTABLE_NAME} PRIVATE
        -sFULL_ES3=1
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=134217728
        -sSTACK_SIZE=2097152
        -sUSE_SDL=3
        --shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html
    )
    # Preload game data if specified
    if(GAME_DATA_DIR)
        target_link_options(${EXECUTABLE_NAME} PRIVATE
            --preload-file=${GAME_DATA_DIR}@/data1
        )
    endif()
    set_target_properties(${EXECUTABLE_NAME} PROPERTIES SUFFIX ".html")
elseif(WIN32)
    set(EXECUTABLE_NAME glh2)
    add_executable(${EXECUTABLE_NAME} WIN32 ${ALL_SOURCES} ${RESOURCES})
else()
    set(EXECUTABLE_NAME glhexen2)
    add_executable(${EXECUTABLE_NAME} ${ALL_SOURCES})
endif()

target_compile_definitions(${EXECUTABLE_NAME} PRIVATE ${CLIENT_DEFINITIONS})

#============================================================================
# Link Libraries
#============================================================================
# SDL is always required
target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${SDL_LIBRARY})
target_include_directories(${EXECUTABLE_NAME} PRIVATE ${SDL_INCLUDE_DIR})

# Platform-specific linking
if(EMSCRIPTEN)
    # Emscripten links GL/SDL statically — no extra libraries needed
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE m)
elseif(WIN32)
    # Windows libraries
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE
        opengl32
        ws2_32
        winmm
    )

    # Codec libraries for Windows (pkg-config or oslibs fallback)
    if(USE_CODEC_VORBIS)
        if(VORBIS_FOUND)
            target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${VORBIS_LIBRARIES})
            target_include_directories(${EXECUTABLE_NAME} PRIVATE ${VORBIS_INCLUDE_DIRS})
        else()
            target_link_libraries(${EXECUTABLE_NAME} PRIVATE vorbisfile vorbis ogg)
        endif()
    endif()
    if(USE_CODEC_XMP AND XMP_FOUND)
        target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${XMP_LIBRARIES})
        target_include_directories(${EXECUTABLE_NAME} PRIVATE ${XMP_INCLUDE_DIRS})
    endif()
    if(USE_CODEC_OPUS AND OPUS_FOUND)
        target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${OPUS_LIBRARIES})
        target_include_directories(${EXECUTABLE_NAME} PRIVATE ${OPUS_INCLUDE_DIRS})
    endif()
elseif(UNIX)
    # OpenGL, or GLESv2 for the ES tier (see USE_GLES above)
    if(USE_GLES)
        target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${GLESV2_LIBRARIES})
        target_include_directories(${EXECUTABLE_NAME} PRIVATE ${GLESV2_INCLUDE_DIRS})
    else()
        target_link_libraries(${EXECUTABLE_NAME} PRIVATE OpenGL::GL)
    endif()

    # System libraries
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${M_LIBRARY})
    if(DL_LIBRARY)
        target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${DL_LIBRARY})
    endif()

    # ALSA
    if(USE_ALSA_FOUND)
        target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${ALSA_LIBRARIES})
        target_include_directories(${EXECUTABLE_NAME} PRIVATE ${ALSA_INCLUDE_DIRS})
    endif()

    # Codec libraries for Linux
    if(USE_CODEC_VORBIS AND VORBIS_FOUND)
        target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${VORBIS_LIBRARIES})
        target_include_directories(${EXECUTABLE_NAME} PRIVATE ${VORBIS_INCLUDE_DIRS})
    endif()
    # FLAC/MP3: dr_flac/dr_mp3 are built-in, no external libraries to link
    if(USE_CODEC_XMP AND XMP_FOUND)
        target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${XMP_LIBRARIES})
        target_include_directories(${EXECUTABLE_NAME} PRIVATE ${XMP_INCLUDE_DIRS})
    endif()
    if(USE_CODEC_OPUS AND OPUS_FOUND)
        target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${OPUS_LIBRARIES})
        target_include_directories(${EXECUTABLE_NAME} PRIVATE ${OPUS_INCLUDE_DIRS})
    endif()
endif()

# FluidSynth MIDI
if(FLUIDSYNTH_FOUND)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${FLUIDSYNTH_LIBRARIES})
    target_include_directories(${EXECUTABLE_NAME} PRIVATE ${FLUIDSYNTH_INCLUDE_DIRS})
endif()

#============================================================================
# Install Targets
#============================================================================
install(TARGETS ${EXECUTABLE_NAME}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

#============================================================================
# Dedicated Server (h2ded)
#============================================================================
# Client-free build: no renderer, no video, no sound, no input.  Everything is
# driven by the SERVERONLY define, which switches quakeinc.h over to the
# BSP-only sv_model.h header set and compiles out the client halves of
# sv_main/sv_user/net_*/pr_*/quakefs/zone/cmd.  This target replaces the
# engine/hexen2/server/Makefile that was dropped in the build-system cleanup.
if(BUILD_DEDICATED)
if(EMSCRIPTEN OR (NOT UNIX AND NOT WIN32))
    # The web target has no use for a server at all.
    message(WARNING "BUILD_DEDICATED is only wired up for Unix and Windows hosts - skipping h2ded")
else()
    # Shares COMMON_SOURCES with the client; the removals below are the parts
    # of that list that cannot compile without a client.
    set(SERVER_SOURCES ${COMMON_SOURCES})
    list(REMOVE_ITEM SERVER_SOURCES
        ${ENGINE_TOP}/hexen2/host.c        # superseded by hexen2/server/host.c
        ${ENGINE_TOP}/hexen2/host_cmd.c    # superseded by hexen2/server/host_cmd.c
        ${ENGINE_TOP}/hexen2/host_async.c  # background-save worker needs SDL threads
        ${ENGINE_TOP}/hexen2/net_loop.c    # loopback driver needs cls/ca_dedicated
        ${COMMONDIR}/wad.c                 # gfx.wad lumps are renderer-only data
    )
    list(APPEND SERVER_SOURCES
        ${ENGINE_TOP}/hexen2/server/host.c
        ${ENGINE_TOP}/hexen2/server/host_cmd.c
        ${COMMONDIR}/sv_model.c            # BSP-only replacement for gl_model.c
    )

    # Network + system layer.  hexen2/server/sys_{unix,win}.c are the SDL-free
    # counterparts of hexen2/sys_{unix,win}.c, so sys_sdl.c is not needed either.
    if(WIN32)
        list(APPEND SERVER_SOURCES
            ${ENGINE_TOP}/hexen2/net_win.c
            ${ENGINE_TOP}/hexen2/net_wins.c
            ${ENGINE_TOP}/hexen2/server/sys_win.c
        )
    else()
        list(APPEND SERVER_SOURCES
            ${ENGINE_TOP}/hexen2/net_bsd.c
            ${ENGINE_TOP}/hexen2/net_udp.c
            ${ENGINE_TOP}/hexen2/server/sys_unix.c
        )
    endif()

    # No WIN32 flag on Windows: h2ded is a console program with a plain main(),
    # and its output is the server console.
    add_executable(h2ded ${SERVER_SOURCES})
    target_compile_definitions(h2ded PRIVATE SERVERONLY)
    if(WIN32)
        target_link_libraries(h2ded PRIVATE ws2_32 winmm)
    else()
        target_link_libraries(h2ded PRIVATE ${M_LIBRARY})
    endif()

    install(TARGETS h2ded RUNTIME DESTINATION bin)

    message(STATUS "Dedicated server: h2ded enabled")
endif()
endif()

#============================================================================
# Summary
#============================================================================
message(STATUS "")
message(STATUS "=== glhexen2 Configuration ===")
message(STATUS "Target OS: ${TARGET_OS}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Sound support: ${USE_SOUND}")
message(STATUS "ALSA: ${USE_ALSA}")
message(STATUS "Vorbis codec: ${USE_CODEC_VORBIS}")
message(STATUS "MP3 codec: ${USE_CODEC_MP3}")
message(STATUS "FLAC codec: ${USE_CODEC_FLAC}")
message(STATUS "FluidSynth MIDI: ${FLUIDSYNTH_FOUND}")
message(STATUS "MIDI fallback codec (libTiMidity): ${TIMIDITY_CODEC_ENABLED}")
message(STATUS "Tracker (libxmp): ${USE_CODEC_XMP}")
message(STATUS "UMX container:    ${USE_CODEC_UMX}")
message(STATUS "Opus codec:       ${USE_CODEC_OPUS}")
message(STATUS "Dedicated server: ${BUILD_DEDICATED}")
message(STATUS "=============================")
message(STATUS "")
