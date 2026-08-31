# CMake build for the Hexen II map/model toolchain.
#
# Replaces the twelve near-identical GNU Makefiles in this tree for day-to-day
# use.  Those Makefiles (and scripts/makefile.inc, scripts/detect.sh) are kept
# for now and remain the authoritative source for every source list, define and
# link library below -- when in doubt, diff against <tool>/Makefile.
#
# Works both standalone (`cmake -S utils -B build`) and as a subdirectory of the
# repo-root superproject (`cmake -S . -DBUILD_UTILS=ON`).

cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(uhexen2-utils LANGUAGES C)
endif()

set(UTILS_TOP ${CMAKE_CURRENT_SOURCE_DIR})
set(UHEXEN2_TOP ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(UHEXEN2_SHARED ${UHEXEN2_TOP}/common)
set(OSLIBS ${UHEXEN2_TOP}/oslibs)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

#============================================================================
# Common flags
#============================================================================
# The Makefiles all use "-Wall -O2 -DNDEBUG=1" (or -g for DEBUG=1), and add
# -march=i586 only on 32-bit x86, which is not a target here.
set(UTIL_C_FLAGS -Wall)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND UTIL_C_FLAGS -g)
else()
    list(APPEND UTIL_C_FLAGS -O2)
    set(UTIL_C_DEFS NDEBUG=1)
endif()

if(WIN32)
    list(APPEND UTIL_C_DEFS WIN32_LEAN_AND_MEAN)
endif()

#============================================================================
# Shared library: utils/common + the handful of repo-root common sources
#============================================================================
# Every Makefile recompiles these same files into its own directory, so they are
# a library in all but name.  Static, so each tool still links only the objects
# it actually references -- e.g. nothing but light/vis/jsh2colour pulls in
# threads.c, and only the tools that call it pull in mathlib.c.
set(UTILS_COMMON_SOURCES
    ${UTILS_TOP}/common/bspfile.c
    ${UTILS_TOP}/common/byteordr.c
    ${UTILS_TOP}/common/cmdlib.c
    ${UTILS_TOP}/common/mathlib.c
    ${UTILS_TOP}/common/pathutil.c
    ${UTILS_TOP}/common/qdir.c
    ${UTILS_TOP}/common/scriplib.c
    ${UTILS_TOP}/common/threads.c
    ${UTILS_TOP}/common/util_io.c
    ${UHEXEN2_SHARED}/qsnprint.c
    ${UHEXEN2_SHARED}/strlcat.c
    ${UHEXEN2_SHARED}/strlcpy.c
    ${UHEXEN2_SHARED}/q_endian.c
    ${UHEXEN2_SHARED}/crc.c
)

if(UNIX)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
endif()

# Two flavours, because DOUBLEVEC_T switches vec_t from float to double
# (common/q_stdinc.h:153).  That is an ABI change reaching through mathlib.h and
# bspfile.h, so qbsp/light/vis/jsh2colour cannot share objects with the rest --
# which is exactly why every Makefile builds its own private copies.
function(add_utils_common_lib name)
    add_library(${name} STATIC ${UTILS_COMMON_SOURCES})
    target_compile_options(${name} PRIVATE ${UTIL_C_FLAGS})
    target_compile_definitions(${name} PUBLIC ${UTIL_C_DEFS} ${ARGN})
    target_include_directories(${name} PUBLIC ${UTILS_TOP}/common ${UHEXEN2_SHARED})
    if(WIN32)
        target_include_directories(${name} PUBLIC ${OSLIBS}/windows/misc/include)
    endif()
    if(UNIX)
        # PUBLIC: threads.c is in this archive, and libm is wanted by every
        # consumer of mathlib.c.  Both are no-ops for tools that reference
        # neither -- a static archive contributes only the members that resolve
        # an undefined symbol.
        target_compile_definitions(${name} PUBLIC USE_PTHREADS)
        target_link_libraries(${name} PUBLIC m Threads::Threads)
    endif()
endfunction()

add_utils_common_lib(utils_common)
add_utils_common_lib(utils_common_dv DOUBLEVEC_T)

#============================================================================
# Tools
#============================================================================
# add_util_tool(<target> <subdir> [FASTMATH] [DOUBLEVEC] SOURCES <src>...)
#
# FASTMATH and DOUBLEVEC mirror each tool's Makefile exactly; see the call sites.
function(add_util_tool target subdir)
    cmake_parse_arguments(T "FASTMATH;DOUBLEVEC" "" "SOURCES" ${ARGN})

    list(TRANSFORM T_SOURCES PREPEND ${UTILS_TOP}/${subdir}/)
    add_executable(${target} ${T_SOURCES})

    target_compile_options(${target} PRIVATE ${UTIL_C_FLAGS})
    if(T_FASTMATH)
        target_compile_options(${target} PRIVATE -ffast-math)
    endif()
    target_include_directories(${target} PRIVATE ${UTILS_TOP}/${subdir})

    if(T_DOUBLEVEC)
        target_link_libraries(${target} PRIVATE utils_common_dv)
    else()
        target_link_libraries(${target} PRIVATE utils_common)
    endif()

    if(WIN32)
        target_link_options(${target} PRIVATE -mconsole)
    endif()

    install(TARGETS ${target} RUNTIME DESTINATION bin)
endfunction()

# hcc -- the HexenC compiler.  No -ffast-math: it constant-folds the float
# literals that end up in progs.dat, and the Makefile withholds it too.
add_util_tool(hcc hcc SOURCES
    crchash.c expr.c hcc.c pr_comp.c pr_lex.c stmt.c)

# dhcc -- Eric Hobbs' progs.dat decompiler.  Its hcc.c/pr_comp.c/pr_lex.c are
# forks of hcc's, not the same files; likewise no -ffast-math.
add_util_tool(dhcc dcc SOURCES
    dcc.c hcc.c pr_comp.c pr_lex.c)

add_util_tool(qbsp qbsp FASTMATH DOUBLEVEC SOURCES
    brush.c csg4.c map.c merge.c nodraw.c outside.c portals.c qbsp.c
    region.c solidbsp.c surfaces.c tjunc.c writebsp.c)

add_util_tool(light light FASTMATH DOUBLEVEC SOURCES
    entities.c light.c ltface.c trace.c)

add_util_tool(vis vis FASTMATH DOUBLEVEC SOURCES
    flow.c soundpvs.c vis.c)

# Binary is "jsh2colour", not "jsh2color" -- build.sh and the Makefile agree.
add_util_tool(jsh2colour jsh2color FASTMATH DOUBLEVEC SOURCES
    litfile.c entities.c ltface.c trace.c jscolor.c tyrlite.c)
if(WIN32)
    # Recursive lightmap tracing overflows the default 2 MB stack (FIXME
    # inherited from jsh2color/Makefile).
    target_link_options(jsh2colour PRIVATE -Wl,--stack,4194304)
endif()

# entsearch -- entity search across .map sources; the design follows Inky's
# MapSearch.  Brings its own regex engine and directory walk: mingw ships no
# <regex.h>, and utils/common's Q_FindFirstFile hides the directories a
# recursive walk has to descend into.
add_util_tool(entsearch entsearch SOURCES
    entsearch.c es_config.c es_map.c es_regex.c)

add_util_tool(bsp2map bsp2map FASTMATH SOURCES bsp2map.c)
add_util_tool(bspinfo bspinfo FASTMATH SOURCES bspinfo.c)
add_util_tool(genmodel genmodel FASTMATH SOURCES genmodel.c loadtri.c token.c)
add_util_tool(qfiles qfiles SOURCES qfiles.c)

# pakfile.c is shared between the two pak tools and lives in pak/, not common/.
add_util_tool(pakx pak SOURCES pakfile.c pakx.c)
add_util_tool(paklist pak SOURCES pakfile.c paklist.c)

add_util_tool(bsp2wal texutils/bsp2wal SOURCES bsp2wal.c)
add_util_tool(lmp2pcx texutils/lmp2pcx SOURCES lmp2pcx.c)

# Not built here, on purpose:
#   texutils/palette/  -- palette/colormap data plus GIMP and Paint Shop Pro
#                         helpers.  No C sources, no Makefile, nothing to build.
#   qbsp/entmap.c      -- present in the tree but absent from qbsp/Makefile's
#                         object list; it is not part of qbsp and defines its
#                         own main().
#   progs_dump.c       -- a loose file at the top of utils/, referenced by no
#                         Makefile and no build script.

message(STATUS "uhexen2 utils: hcc dhcc qbsp light vis jsh2colour bsp2map bspinfo "
               "genmodel entsearch qfiles pakx paklist bsp2wal lmp2pcx")
