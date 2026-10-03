# Dependencies for playback-client.
#
# Everything is built from pinned source, for the same reasons as
# rendering-engine (docs/adr/0001-build-ladybug-from-pinned-source.md): the
# component must build for macOS, Linux and Windows, and a system package
# would break cross-compilation. The pins for the libraries the two
# components share (CLI11, cpp-httplib, nlohmann/json, dr_libs) are the same
# as rendering-engine's; bump them together.
#
# Exposes:
#   rainbohz::cli11     -- command line parsing (header-only)
#   rainbohz::httplib   -- HTTP client for the REST calls (header-only)
#   rainbohz::json      -- JSON for the REST bodies (header-only)
#   rainbohz::dr_wav    -- WAV reading and writing (header-only)
#   rainbohz::miniaudio -- audio output to the default device (header-only)

include(FetchContent)

# ---------------------------------------------------------------------------
# CLI11
# ---------------------------------------------------------------------------

set(PC_CLI11_GIT_REPOSITORY "https://github.com/CLIUtils/CLI11"
    CACHE STRING "CLI11 source repository")
set(PC_CLI11_GIT_TAG "v2.7.2" CACHE STRING "CLI11 version to build against")

set(CLI11_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(CLI11_SINGLE_FILE    OFF CACHE BOOL "" FORCE)
set(CLI11_INSTALL        OFF CACHE BOOL "" FORCE)
set(CLI11_PRECOMPILED    OFF CACHE BOOL "" FORCE)

message(STATUS "playback-client: fetching CLI11 ${PC_CLI11_GIT_TAG}")
FetchContent_Declare(cli11
    GIT_REPOSITORY "${PC_CLI11_GIT_REPOSITORY}"
    GIT_TAG        "${PC_CLI11_GIT_TAG}"
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(cli11)

if(NOT TARGET CLI11::CLI11)
    message(FATAL_ERROR
        "CLI11 was configured but the 'CLI11::CLI11' target does not exist. "
        "The upstream target layout may have changed.")
endif()

# Our warning set is aimed at code we own.
get_target_property(PC_CLI11_INCLUDES CLI11 INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(CLI11 PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${PC_CLI11_INCLUDES}")

add_library(rainbohz::cli11 ALIAS CLI11)

# ---------------------------------------------------------------------------
# cpp-httplib
# ---------------------------------------------------------------------------
#
# The HTTP client for the REST calls. Upstream's CMakeLists is not run, so
# that HTTPS and compression stay off: the client speaks plain HTTP to a
# server that sits behind a gateway (rendering-engine/docs/server.md). The
# header is exposed as an INTERFACE target and linked PRIVATE by
# src/playback_api.cpp's library alone; no httplib type appears in a public
# header.
#
# Unlike rendering-engine, this component does not link Ladybug, so httplib
# is used under its own namespace (rendering-engine renames it; ADR 0016).

set(PC_HTTPLIB_GIT_REPOSITORY "https://github.com/yhirose/cpp-httplib"
    CACHE STRING "cpp-httplib source repository")
set(PC_HTTPLIB_GIT_TAG "v0.58.0" CACHE STRING "cpp-httplib version to build against")

message(STATUS "playback-client: fetching cpp-httplib ${PC_HTTPLIB_GIT_TAG}")
FetchContent_Declare(httplib
    GIT_REPOSITORY "${PC_HTTPLIB_GIT_REPOSITORY}"
    GIT_TAG        "${PC_HTTPLIB_GIT_TAG}"
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SOURCE_SUBDIR  do-not-configure
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(httplib)

if(NOT EXISTS "${httplib_SOURCE_DIR}/httplib.h")
    message(FATAL_ERROR
        "cpp-httplib was fetched but ${httplib_SOURCE_DIR}/httplib.h does not "
        "exist. The upstream layout may have changed.")
endif()

find_package(Threads REQUIRED)
add_library(httplib INTERFACE)
target_include_directories(httplib SYSTEM INTERFACE "${httplib_SOURCE_DIR}")
target_link_libraries(httplib INTERFACE
    Threads::Threads
    $<$<PLATFORM_ID:Windows>:ws2_32>
)
add_library(rainbohz::httplib ALIAS httplib)

# ---------------------------------------------------------------------------
# nlohmann/json
# ---------------------------------------------------------------------------

set(PC_JSON_GIT_REPOSITORY "https://github.com/nlohmann/json"
    CACHE STRING "nlohmann/json source repository")
set(PC_JSON_GIT_TAG "v3.12.0" CACHE STRING "nlohmann/json version to build against")

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)

message(STATUS "playback-client: fetching nlohmann/json ${PC_JSON_GIT_TAG}")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY "${PC_JSON_GIT_REPOSITORY}"
    GIT_TAG        "${PC_JSON_GIT_TAG}"
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(nlohmann_json)

if(NOT TARGET nlohmann_json::nlohmann_json)
    message(FATAL_ERROR
        "nlohmann/json was configured but the 'nlohmann_json::nlohmann_json' "
        "target does not exist. The upstream target layout may have changed.")
endif()

get_target_property(PC_JSON_INCLUDES nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(nlohmann_json PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${PC_JSON_INCLUDES}")

add_library(rainbohz::json ALIAS nlohmann_json)

# ---------------------------------------------------------------------------
# dr_wav
# ---------------------------------------------------------------------------
#
# Reads the frames rendering-engine writes (32-bit float WAV), and writes the
# --output-wav file. The same commit as rendering-engine's (ADR 0009). Linked
# PRIVATE by src/wav_io.cpp alone, which carries the implementation.

set(PC_DR_LIBS_GIT_REPOSITORY "https://github.com/mackron/dr_libs"
    CACHE STRING "dr_libs source repository")
set(PC_DR_LIBS_GIT_TAG "dfe8377631000664666519fdb83da193fd8037f4"
    CACHE STRING "dr_libs commit to build against")

message(STATUS "playback-client: fetching dr_libs ${PC_DR_LIBS_GIT_TAG}")
FetchContent_Declare(dr_libs
    GIT_REPOSITORY "${PC_DR_LIBS_GIT_REPOSITORY}"
    GIT_TAG        "${PC_DR_LIBS_GIT_TAG}"
    GIT_PROGRESS   TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(dr_libs)

if(NOT EXISTS "${dr_libs_SOURCE_DIR}/dr_wav.h")
    message(FATAL_ERROR
        "dr_libs was fetched but ${dr_libs_SOURCE_DIR}/dr_wav.h does not exist. "
        "The upstream layout may have changed.")
endif()

add_library(dr_wav INTERFACE)
target_include_directories(dr_wav SYSTEM INTERFACE "${dr_libs_SOURCE_DIR}")
add_library(rainbohz::dr_wav ALIAS dr_wav)

# ---------------------------------------------------------------------------
# miniaudio
# ---------------------------------------------------------------------------
#
# Plays audio on the operating system's default output device: Core Audio on
# macOS, ALSA/PulseAudio on Linux, WASAPI on Windows. One C header from
# mackron/miniaudio, public domain or MIT-0; chosen in
# docs/adr/0018-play-audio-with-miniaudio.md.
#
# Pinned to a release tag. Check for newer releases with:
#
#   git ls-remote --tags --refs https://github.com/mackron/miniaudio
#
# Upstream's CMakeLists is not run: it builds the library, its examples and
# its extras, where only the header is wanted. Linked PRIVATE by
# src/device_sink.cpp alone, which carries the implementation; no miniaudio
# type appears in a public header.

set(PC_MINIAUDIO_GIT_REPOSITORY "https://github.com/mackron/miniaudio"
    CACHE STRING "miniaudio source repository")
set(PC_MINIAUDIO_GIT_TAG "0.11.25" CACHE STRING "miniaudio version to build against")

message(STATUS "playback-client: fetching miniaudio ${PC_MINIAUDIO_GIT_TAG}")
FetchContent_Declare(miniaudio
    GIT_REPOSITORY "${PC_MINIAUDIO_GIT_REPOSITORY}"
    GIT_TAG        "${PC_MINIAUDIO_GIT_TAG}"
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    SOURCE_SUBDIR  do-not-configure
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(miniaudio)

if(NOT EXISTS "${miniaudio_SOURCE_DIR}/miniaudio.h")
    message(FATAL_ERROR
        "miniaudio was fetched but ${miniaudio_SOURCE_DIR}/miniaudio.h does not "
        "exist. The upstream layout may have changed.")
endif()

# miniaudio loads its backends at run time with dlopen on Linux, and links the
# Apple frameworks it needs on macOS.
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio SYSTEM INTERFACE "${miniaudio_SOURCE_DIR}")
target_link_libraries(miniaudio INTERFACE
    Threads::Threads
    $<$<PLATFORM_ID:Linux>:${CMAKE_DL_LIBS}>
    $<$<PLATFORM_ID:Linux>:m>
    "$<$<PLATFORM_ID:Darwin>:-framework CoreFoundation>"
    "$<$<PLATFORM_ID:Darwin>:-framework CoreAudio>"
    "$<$<PLATFORM_ID:Darwin>:-framework AudioToolbox>"
)
add_library(rainbohz::miniaudio ALIAS miniaudio)
