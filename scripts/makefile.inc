# GNU Makefile using GCC in a unix shell.

# This file is responsible for determining the Host OS and Target OS,
# selecting the compiler, assigning the compiler command as the linker,
# defining helper functions for compiler flags detection.

# Common unix shell tools, such as uname, which, sed and tr, are required
# for successful operation. If you are running on windows, you must run
# under MSYS, otherwise this makefile system will fail.

################################
# Helper function to check availability of a given command
check_cmd=$(shell if which $(1) > /dev/null 2>&1; then echo "";else echo $(1);fi)

################################
# OS and TARGET_OS detection:

CROSS_COMPILE=n

HOST_OS0=$(shell uname)
HOST_OS =$(shell uname|sed -e s/_.*//|tr '[:upper:]' '[:lower:]')

# NOTE:	We only support these types of cross compilation:
#	Compile for windows on unix.
#	This means, unless cross compiling, the actual
#	TARGET_OS remains as the HOST_OS.

ifneq (,$(findstring mingw32,$(HOST_OS)))
HOST_OS =windows
endif

ifeq ($(HOST_OS),windows)
# running on Windows

# compiling for windows
ifdef W64BUILD
CROSS_COMPILE=y
TARGET_OS=win64
else
TARGET_OS=win32
endif
endif #

ifneq ($(HOST_OS),windows)
# assuming a unix host

ifdef W32BUILD
# cross compilation for windows
CROSS_COMPILE=y
TARGET_OS=win32
endif
ifdef W64BUILD
# cross compilation for windows
CROSS_COMPILE=y
TARGET_OS=win64
endif
ifeq ($(CROSS_COMPILE),n)
# compiling for the host os.
TARGET_OS=unix
endif

endif #

ifeq ($(TARGET_OS),unix)
MACH_TYPE=$(shell sh $(UHEXEN2_TOP)/scripts/detect.sh arch)
endif
ifeq ($(TARGET_OS),win32)
# FIXME: win32 does not have to be x86.
MACH_TYPE=x86
endif
ifeq ($(TARGET_OS),win64)
# FIXME: uname from msys might not work here
MACH_TYPE=x86_64
endif

################################
# Binary filename extension:

exe_ext=
ifeq ($(TARGET_OS),win32)
exe_ext=.exe
endif
ifeq ($(TARGET_OS),win64)
exe_ext=.exe
endif

################################
# Compiler selection:

CC ?= gcc
ifeq ($(origin CC),default)
# refuse the stupid 'cc' default of make
# which not necessarily exist everywhere
CC  = gcc
endif
AS ?= as
AR ?= ar
RANLIB ?= ranlib
NASM ?= nasm
WINDRES ?= windres
RC ?= $(WINDRES)
LINKER = $(CC)
