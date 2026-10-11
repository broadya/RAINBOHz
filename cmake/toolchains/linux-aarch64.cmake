# SPDX-License-Identifier: Apache-2.0

# Cross-compile a RAINBOHz C++ component for Linux/arm64 (AWS Graviton).
#
# STATUS: scaffolding. This file records the intended cross-compile
# configuration. It has not been exercised against a real sysroot, and
# Ladybug's vendored third-party tree is large enough that first use should be
# expected to need adjustment.
#
# Requires a sysroot -- a copy of the target's headers and libraries. Obtain
# one however you like (debootstrap, a container filesystem export, a vendor
# sysroot tarball) and pass its path:
#
#   cmake --preset linux-aarch64-cross -DRAINBOHZ_SYSROOT=/opt/sysroots/aarch64-linux-gnu
#
# The clang driver is used because a single clang installation targets every
# architecture; only the sysroot changes. Homebrew LLVM (not Apple clang) is
# needed on macOS, since Apple's build does not ship Linux targets.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(RAINBOHZ_SYSROOT "" CACHE PATH "Path to the aarch64 Linux sysroot")
if(NOT RAINBOHZ_SYSROOT)
    message(FATAL_ERROR
        "linux-aarch64.cmake requires -DRAINBOHZ_SYSROOT=<path to an aarch64 Linux sysroot>. "
        "See the comments in this file.")
endif()

set(RAINBOHZ_TARGET_TRIPLE aarch64-unknown-linux-gnu)

find_program(RAINBOHZ_CLANG   NAMES clang   HINTS /opt/homebrew/opt/llvm/bin /usr/lib/llvm/bin REQUIRED)
find_program(RAINBOHZ_CLANGXX NAMES clang++ HINTS /opt/homebrew/opt/llvm/bin /usr/lib/llvm/bin REQUIRED)

set(CMAKE_C_COMPILER   "${RAINBOHZ_CLANG}")
set(CMAKE_CXX_COMPILER "${RAINBOHZ_CLANGXX}")

set(CMAKE_C_COMPILER_TARGET   ${RAINBOHZ_TARGET_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${RAINBOHZ_TARGET_TRIPLE})
set(CMAKE_SYSROOT "${RAINBOHZ_SYSROOT}")

# lld is the only linker reliably able to link a foreign target from a macOS
# host; the system ld64 cannot produce ELF.
set(RAINBOHZ_LINKER_FLAGS "-fuse-ld=lld --target=${RAINBOHZ_TARGET_TRIPLE} --sysroot=${RAINBOHZ_SYSROOT}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${RAINBOHZ_LINKER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${RAINBOHZ_LINKER_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${RAINBOHZ_LINKER_FLAGS}")

# Graviton2 and later are Neoverse cores implementing at least ARMv8.2-A.
# Leave this conservative: the render path, not the database layer, is the component
# that will want aggressive architecture tuning.
set(CMAKE_C_FLAGS_INIT   "-march=armv8.2-a")
set(CMAKE_CXX_FLAGS_INIT "-march=armv8.2-a")

# Look for target libraries only in the sysroot; never pick up host artefacts.
set(CMAKE_FIND_ROOT_PATH "${RAINBOHZ_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
