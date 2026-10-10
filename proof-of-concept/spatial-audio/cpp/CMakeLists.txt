# SPDX-License-Identifier: Apache-2.0

# 3.28 for FetchContent's EXCLUDE_FROM_ALL, as rendering-engine.
cmake_minimum_required(VERSION 3.28)

# spatial-audio C++ PoC
#
# Per-channel gains of a point source for each of the engine's channel modes
# (EngineConfig.channelMode): perfect, coincident, first-order virtual
# microphones. A port of the Python oracle beside it (../src/spatial_poc),
# checked against it by a golden CSV. Standalone: it depends on nothing in
# rendering-engine, and is not wired into the repository's Makefile.

project(spatial_poc
    VERSION 0.1.0
    DESCRIPTION "RAINBOHz spatial-audio PoC: virtual microphone gains"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(SP_BUILD_TESTS "Build spatial-audio PoC tests" ON)
option(SP_ENABLE_WERROR "Treat warnings as errors" OFF)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# The rendering-engine warning set (rendering-engine/cmake/ProjectWarnings.cmake),
# GCC/Clang only: this PoC is not built with MSVC.
add_library(sp_warnings INTERFACE)
if(NOT MSVC)
    target_compile_options(sp_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
        -Wcast-align -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
        -Wdouble-promotion -Wnull-dereference -Wformat=2 -Wimplicit-fallthrough)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(sp_warnings INTERFACE -Wdocumentation)
    endif()
    if(SP_ENABLE_WERROR)
        target_compile_options(sp_warnings INTERFACE -Werror)
    endif()
endif()

add_library(spatial_poc STATIC
    src/channel_mode.cpp
    src/spatial_gains.cpp
)
target_include_directories(spatial_poc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(spatial_poc PRIVATE sp_warnings)

if(SP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
