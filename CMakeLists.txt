# Superproject for uHexen2 / Hexenwail.
#
# Lets the engine and the map/model toolchain be configured from one place:
#   cmake -S . -B build                    # engine only (what it has always been)
#   cmake -S . -B build -DBUILD_UTILS=ON   # engine + utils/ + hw_utils/
#
# This file deliberately sets no compile options, no definitions and no include
# directories.  Anything set here would be inherited by every subdirectory, and
# the two trees are not flag-compatible: utils/common/mathlib.h and
# engine/h2shared/mathlib.h share a filename, so a leaked -I from either side
# would silently shadow the other's header.  The engine also compiles with
# -ffast-math throughout, which the utils Makefiles deliberately withhold from
# the bytecode compilers (hcc, dhcc).  Each subdirectory owns its flags outright.
#
# engine/CMakeLists.txt remains a valid standalone root, and `cmake -S engine`
# is still exactly what the flake's client, h2ded and win64 outputs configure --
# so adding this file cannot change what any of them produce.

cmake_minimum_required(VERSION 3.16)
project(uhexen2 LANGUAGES C)

option(BUILD_ENGINE "Build the Hexen II engine (client, plus h2ded via BUILD_DEDICATED)" ON)

# Off by default so a plain configure of this directory keeps meaning "the
# engine", matching the BUILD_DEDICATED precedent in engine/CMakeLists.txt.
option(BUILD_UTILS "Build the map/model toolchain (utils/) and HexenWorld servers (hw_utils/)" OFF)

if(NOT BUILD_ENGINE AND NOT BUILD_UTILS)
    message(FATAL_ERROR "Nothing to build: BUILD_ENGINE and BUILD_UTILS are both OFF")
endif()

if(BUILD_ENGINE)
    add_subdirectory(engine)
endif()

if(BUILD_UTILS)
    add_subdirectory(utils)
    add_subdirectory(hw_utils)
endif()
