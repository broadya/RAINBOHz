# Cross-compile rendering-engine for Linux/x86_64.
#
# STATUS: scaffolding, same caveats as linux-aarch64.cmake -- see that file for
# the full explanation of the sysroot requirement and the choice of clang+lld.
#
#   cmake --preset linux-x86_64-cross -DRE_SYSROOT=/opt/sysroots/x86_64-linux-gnu
#
# x86_64 is a portability check, not a deployment target: production is
# Graviton. Keeping this configuration alive stops architecture assumptions
# from quietly accumulating in the code.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(RE_SYSROOT "" CACHE PATH "Path to the x86_64 Linux sysroot")
if(NOT RE_SYSROOT)
    message(FATAL_ERROR
        "linux-x86_64.cmake requires -DRE_SYSROOT=<path to an x86_64 Linux sysroot>. "
        "See the comments in this file.")
endif()

set(RE_TARGET_TRIPLE x86_64-unknown-linux-gnu)

find_program(RE_CLANG   NAMES clang   HINTS /opt/homebrew/opt/llvm/bin /usr/lib/llvm/bin REQUIRED)
find_program(RE_CLANGXX NAMES clang++ HINTS /opt/homebrew/opt/llvm/bin /usr/lib/llvm/bin REQUIRED)

set(CMAKE_C_COMPILER   "${RE_CLANG}")
set(CMAKE_CXX_COMPILER "${RE_CLANGXX}")

set(CMAKE_C_COMPILER_TARGET   ${RE_TARGET_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${RE_TARGET_TRIPLE})
set(CMAKE_SYSROOT "${RE_SYSROOT}")

set(RE_LINKER_FLAGS "-fuse-ld=lld --target=${RE_TARGET_TRIPLE} --sysroot=${RE_SYSROOT}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${RE_LINKER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${RE_LINKER_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${RE_LINKER_FLAGS}")

# x86-64-v2 is the practical floor for any current cloud instance.
set(CMAKE_C_FLAGS_INIT   "-march=x86-64-v2")
set(CMAKE_CXX_FLAGS_INIT "-march=x86-64-v2")

set(CMAKE_FIND_ROOT_PATH "${RE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
