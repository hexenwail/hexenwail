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
option(USE_SOUND "Enable sound support" ON)
option(USE_ALSA "Enable ALSA audio (Linux)" ON)
option(USE_CODEC_VORBIS "Enable Ogg Vorbis codec" ON)
option(USE_CODEC_MP3 "Enable MP3 codec (dr_mp3)" ON)
option(USE_CODEC_FLAC "Enable FLAC codec" ON)
option(USE_CODEC_XMP "Enable tracker music via libxmp" ON)
option(USE_CODEC_UMX "Enable Unreal UMX container support" ON)
option(USE_CODEC_OPUS "Enable Opus codec" ON)

#============================================================================
# Platform Detection
#============================================================================
if(WIN32)
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
# Compiler Flags
#============================================================================
# Common flags
add_compile_options(-Wall)

if(USE_DEBUG)
    add_compile_options(-g -DDEBUG=1 -DDEBUG_BUILD=1)
else()
    add_compile_options(-O2 -DNDEBUG=1 -ffast-math -fomit-frame-pointer)
endif()

# Platform-specific defines
if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -DGLQUAKE -DSDLQUAKE -DGL_DLSYM -DNO_SPLASHES)
    set(USE_ALSA OFF)
elseif(UNIX)
    add_definitions(-DGLQUAKE -DSDLQUAKE -D_GNU_SOURCE=1 -D_REENTRANT -DGL_DLSYM)
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

if(UNIX)
    # OpenGL
    find_package(OpenGL REQUIRED)

    # Find packages using pkg-config
    find_package(PkgConfig QUIET)
    if(NOT PKG_CONFIG_FOUND)
        message(FATAL_ERROR "pkg-config is required for Linux builds")
    endif()

    # ALSA
    if(USE_ALSA)
        pkg_check_modules(ALSA REQUIRED alsa)
        set(USE_ALSA_FOUND ON)
    endif()

    # Audio codecs (use system libraries on Linux)
    if(USE_CODEC_VORBIS)
        pkg_check_modules(VORBIS REQUIRED vorbisfile vorbis ogg)
    endif()

    # MP3: uses built-in dr_mp3 (no external library needed)

    # FLAC: uses built-in dr_flac (no external library needed)

    # m and dl
    find_library(M_LIBRARY m)
    find_library(DL_LIBRARY dl)
endif()


if(WIN32)
    # For Windows, use prebuilt codecs from oslibs
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
    ${COMMONDIR}/gl_texmgr.c
    ${COMMONDIR}/gl_postprocess.c
    ${COMMONDIR}/gl_matrix.c
    ${COMMONDIR}/gl_shader.c
    ${COMMONDIR}/gl_vbo.c
    ${COMMONDIR}/image.c
    ${COMMONDIR}/img_load.c
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
)

# Server and common sources
set(COMMON_SOURCES
    # From hexen2
    ${ENGINE_TOP}/hexen2/host.c
    ${ENGINE_TOP}/hexen2/host_cmd.c
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
if(WIN32)
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

    # MIDI (FluidSynth on Linux, with null fallback)
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FLUIDSYNTH fluidsynth)
        if(USE_CODEC_XMP)
            pkg_check_modules(XMP REQUIRED libxmp)
        endif()
        if(USE_CODEC_OPUS)
            pkg_check_modules(OPUS REQUIRED opusfile)
        endif()
    endif()
    if(FLUIDSYNTH_FOUND)
        list(APPEND PLATFORM_SOURCES ${COMMONDIR}/midi_fluid.c)
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
# Create Executable
#============================================================================
# Combine all sources
set(ALL_SOURCES
    ${GL_RENDERER_SOURCES}
    ${CLIENT_SOURCES}
    ${COMMON_SOURCES}
    ${SOUND_SOURCES}
    ${PLATFORM_SOURCES}
)

# Create the executable
if(WIN32)
    set(EXECUTABLE_NAME glh2)
    add_executable(${EXECUTABLE_NAME} WIN32 ${ALL_SOURCES} ${RESOURCES})
else()
    set(EXECUTABLE_NAME glhexen2)
    add_executable(${EXECUTABLE_NAME} ${ALL_SOURCES})
endif()

#============================================================================
# Link Libraries
#============================================================================
# SDL is always required
target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${SDL_LIBRARY})
target_include_directories(${EXECUTABLE_NAME} PRIVATE ${SDL_INCLUDE_DIR})

# Platform-specific linking
if(WIN32)
    # Windows libraries
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE
        opengl32
        ws2_32
        winmm
    )

    # Codec libraries for Windows
    if(USE_CODEC_VORBIS)
        target_link_libraries(${EXECUTABLE_NAME} PRIVATE vorbisfile vorbis ogg)
    endif()
elseif(UNIX)
    # OpenGL
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE OpenGL::GL)

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
    if(USE_CODEC_VORBIS)
        target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${VORBIS_LIBRARIES})
        target_include_directories(${EXECUTABLE_NAME} PRIVATE ${VORBIS_INCLUDE_DIRS})
    endif()
    # MP3: dr_mp3 is built-in, no external library to link
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
message(STATUS "Tracker (libxmp): ${USE_CODEC_XMP}")
message(STATUS "UMX container:    ${USE_CODEC_UMX}")
message(STATUS "Opus codec:       ${USE_CODEC_OPUS}")
message(STATUS "=============================")
message(STATUS "")
