# CMake build for the HexenWorld server-side tools.
#
# Separate from utils/CMakeLists.txt because these share nothing with the map
# toolchain: they do not use utils/common at all (only the repo-root common/),
# and they link sockets rather than libm.  The per-tool GNU Makefiles are kept
# for now and remain authoritative for the source lists below.
#
# qstat-patch/ and xqf-patch/ are deliberately absent: they are patch files
# against third-party programs (qstat, XQF), not programs of ours to build.

cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(uhexen2-hw-utils LANGUAGES C)
endif()

set(HW_UTILS_TOP ${CMAKE_CURRENT_SOURCE_DIR})
set(UHEXEN2_TOP ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(UHEXEN2_SHARED ${UHEXEN2_TOP}/common)
set(OSLIBS ${UHEXEN2_TOP}/oslibs)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(HW_C_FLAGS -Wall)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND HW_C_FLAGS -g)
else()
    list(APPEND HW_C_FLAGS -O2)
    set(HW_C_DEFS NDEBUG=1)
endif()

# add_hw_tool(<target> <subdir> SOURCES <src>...)
function(add_hw_tool target subdir)
    cmake_parse_arguments(T "" "" "SOURCES" ${ARGN})

    list(TRANSFORM T_SOURCES PREPEND ${HW_UTILS_TOP}/${subdir}/)
    # qsnprint.c is the only repo-root common source these tools use.
    add_executable(${target} ${T_SOURCES} ${UHEXEN2_SHARED}/qsnprint.c)

    target_compile_options(${target} PRIVATE ${HW_C_FLAGS})
    target_compile_definitions(${target} PRIVATE ${HW_C_DEFS})
    target_include_directories(${target} PRIVATE
        ${HW_UTILS_TOP}/${subdir}
        ${UHEXEN2_SHARED}
    )

    if(WIN32)
        # _USE_WINSOCK2 selects ws2_32 over the legacy wsock32; the Makefiles
        # default to wsock32, but nothing here needs the Winsock 1.1 fallback.
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN _USE_WINSOCK2)
        target_include_directories(${target} PRIVATE ${OSLIBS}/windows/misc/include)
        target_link_libraries(${target} PRIVATE ws2_32 winmm)
        target_link_options(${target} PRIVATE -mconsole)
    endif()

    install(TARGETS ${target} RUNTIME DESTINATION bin)
endfunction()

if(WIN32)
    set(HWMASTER_SYS sys_win.c)
else()
    set(HWMASTER_SYS sys_unix.c)
endif()

add_hw_tool(hwmaster hwmaster SOURCES
    sizebuf.c msg_io.c cmds.c common.c net.c master.c ${HWMASTER_SYS})

add_hw_tool(hwmquery hwmquery SOURCES hwmquery.c)

add_hw_tool(hwrcon hwrcon SOURCES hwrcon.c)
add_hw_tool(hwterm hwrcon SOURCES huffman.c hwterm.c)

message(STATUS "uhexen2 hw_utils: hwmaster hwmquery hwrcon hwterm")
