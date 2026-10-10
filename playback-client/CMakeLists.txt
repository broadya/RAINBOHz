# SPDX-License-Identifier: Apache-2.0

# 3.28 for FetchContent's EXCLUDE_FROM_ALL, as rendering-engine.
cmake_minimum_required(VERSION 3.28)

# playback-client
#
# The component identified as `playback-client` in architecture/overview.md:
# a simple command-line client for playback outside a DAW, kept as a
# demonstration and as a test component. This first version talks to
# rendering-engine's REST service directly, standing in for playback-server,
# and loops a musical range through the default audio device, rendering every
# loop afresh. See docs/design.md.

project(playback_client
    VERSION 0.1.0
    DESCRIPTION "RAINBOHz playback client"
    LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Standard and global settings
# ---------------------------------------------------------------------------

# C++20, as rendering-engine (AGENTS.md, C++ Conventions).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(PC_BUILD_TESTS "Build playback-client tests" ON)
option(PC_ENABLE_WERROR "Treat warnings as errors in playback-client code" OFF)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "playback-client: ccache enabled (${CCACHE_PROGRAM})")
endif()

# Python: one interpreter for the whole repository, the root virtualenv that
# `make venv` creates (AGENTS.md, Python Conventions); the end-to-end
# test runs with it. Ladybug's build (via rendering-engine's
# Dependencies) also looks Python up, for code
# generation that needs only the standard library and any Python 3.9+. It
# states no preference of its own, so it gets the same interpreter: CMake
# caches one for the whole configure. Set before Dependencies so that it
# reaches Ladybug's lookup. Override with -DPython3_EXECUTABLE=... .
if(NOT DEFINED Python3_EXECUTABLE)
    if(CMAKE_HOST_WIN32)
        cmake_path(SET _venv_python NORMALIZE
            "${CMAKE_CURRENT_SOURCE_DIR}/../.venv/Scripts/python.exe")
    else()
        cmake_path(SET _venv_python NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/../.venv/bin/python")
    endif()
    if(NOT EXISTS "${_venv_python}")
        message(FATAL_ERROR "playback-client: no Python virtualenv at ${_venv_python}. "
            "Run `make venv` at the repository root, or pass -DPython3_EXECUTABLE=...")
    endif()
    set(Python3_EXECUTABLE "${_venv_python}"
        CACHE FILEPATH "The Python the build and its tests use (the root .venv)")
endif()

include(ProjectWarnings)
include(Dependencies)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

# The component's logic lives in a library so that tests can link it without
# going through main(). Sources are listed explicitly, never globbed.
add_library(playback_client_lib STATIC
    src/audio_sink.cpp
    src/buffer_dir.cpp
    src/device_sink.cpp
    src/feeder.cpp
    src/frame_plan.cpp
    src/lane_board.cpp
    src/log.cpp
    src/musical_position.cpp
    src/network_worker.cpp
    src/object_fetch.cpp
    src/options.cpp
    src/playback_api.cpp
    src/playback_session.cpp
    src/wav_io.cpp
)
add_library(rainbohz::playback_client_lib ALIAS playback_client_lib)

target_include_directories(playback_client_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(playback_client_lib
    PUBLIC  Threads::Threads
    # No third-party type appears in a public header, so every library stays
    # out of the interface: CLI11 (src/options.cpp), cpp-httplib and
    # nlohmann/json (src/playback_api.cpp), dr_wav (src/wav_io.cpp) and
    # miniaudio (src/device_sink.cpp).
    PRIVATE rainbohz::cli11 rainbohz::httplib rainbohz::json rainbohz::dr_wav
            rainbohz::miniaudio rainbohz::project_warnings
)

add_executable(playback-client src/main.cpp)
target_link_libraries(playback-client
    PRIVATE rainbohz::playback_client_lib rainbohz::project_warnings
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PC_BUILD_TESTS)
    include(CTest)
    add_subdirectory(test)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
install(TARGETS playback-client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
