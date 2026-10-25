# SPDX-License-Identifier: Apache-2.0

# Dependencies for rendering-engine.
#
# Everything is built from source. That is a deliberate choice: the component
# must be buildable for macOS/arm64, Linux/arm64 (Graviton), Linux/x86_64 and
# Windows, and prebuilt Ladybug archives exist only per-platform. Source builds
# follow whatever toolchain CMake is pointed at.
#
# Exposes:
#   rainbohz::ladybug   -- the embedded Ladybug graph database (static)
#   rainbohz::cli11     -- command line parsing (header-only)
#   rainbohz::yaml_cpp  -- YAML parsing, for the batch-mode render request (static)
#   rainbohz::dr_wav    -- WAV file writing (header-only)
#   rainbohz::httplib   -- HTTP server for server mode (header-only)
#   rainbohz::json      -- JSON for server mode's request and response bodies
#                          (header-only)
#   rainbohz::highway   -- portable SIMD for the render kernel (static)
#   rainbohz::parquet   -- Apache Arrow's Parquet reader, for the native backend's
#                          table loader (static)

include(FetchContent)

# ---------------------------------------------------------------------------
# Ladybug
# ---------------------------------------------------------------------------

# The intention is to track the latest stable Ladybug release, but to do so by
# moving this pin deliberately rather than by floating. Ladybug is a community
# fork of an archived project (see AGENTS.md, "Survivability"); the case for
# depending on it assumes we know exactly which source is compiled in, and a
# floating ref would give that up. Check for newer releases with:
#
#   git ls-remote --tags --refs https://github.com/LadybugDB/ladybug
#
# Record behaviour changes across a bump in docs/adr/.
#
# The build uses our fork, not upstream: an upstream release tag plus the
# patches we carry, on a branch rainbohz/<upstream tag> and tagged
# <upstream tag>-rainbohz.<n>. Today that is upstream's own SERIAL primary-key
# index fix (LadybugDB/ladybug#1028), backported because it is merged but not
# yet released (docs/adr/0011-carry-upstreams-serial-fix-until-released.md).
# When a release includes #1028, point this back at LadybugDB/ladybug and
# that release's tag; the fork is then no longer needed.
#
# These are cache variables, so an existing build tree keeps its old values
# after this file changes. Reconfigure from a clean build tree after a bump.
set(RE_LADYBUG_GIT_REPOSITORY "https://github.com/broadya/ladybug"
    CACHE STRING "Ladybug source repository")
set(RE_LADYBUG_GIT_TAG "v0.20.4-rainbohz.2"
    CACHE STRING "Ladybug version to build against")

# Point this at an existing checkout to skip the clone entirely -- useful when
# iterating against a local Ladybug working tree. Not set by default: the
# default build must be reproducible on a clean machine.
set(RE_LADYBUG_SOURCE_DIR "" CACHE PATH
    "Existing Ladybug checkout to build in-tree instead of fetching")

# Ladybug's vendored third-party code still declares cmake_minimum_required
# as low as 3.4, which CMake >= 4.0 rejects outright. This restores the old
# compatibility floor for the dependency subtree only.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0 AND NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

# Trim Ladybug to what an embedded host actually needs: the library, nothing
# else. The shell, language bindings, tests, benchmarks and the generated
# single-file header are all dead weight here, and the single-file header
# additionally drags in a Python 3.9+ requirement at configure time.
set(BUILD_LBUG               ON  CACHE BOOL "" FORCE)
set(BUILD_SHELL              OFF CACHE BOOL "" FORCE)
set(BUILD_SINGLE_FILE_HEADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS              OFF CACHE BOOL "" FORCE)
set(BUILD_EXTENSION_TESTS    OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARK          OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES           OFF CACHE BOOL "" FORCE)
set(BUILD_WAL_DUMP           OFF CACHE BOOL "" FORCE)
set(BUILD_JAVA               OFF CACHE BOOL "" FORCE)
set(BUILD_NODEJS             OFF CACHE BOOL "" FORCE)
set(BUILD_PYTHON             OFF CACHE BOOL "" FORCE)
set(BUILD_EXTENSIONS         ""  CACHE STRING "" FORCE)

# Backtraces on uncaught exceptions. Cheap to have in a long-running server
# process, but it pulls in cpptrace, so it stays opt-in.
option(RE_LADYBUG_BACKTRACES "Build Ladybug with backtrace support" OFF)
set(ENABLE_BACKTRACES ${RE_LADYBUG_BACKTRACES} CACHE BOOL "" FORCE)

# OpenSSL is a hard, unconditional requirement of Ladybug from v0.19.0 onward:
# its CMakeLists does find_package(OpenSSL 3 REQUIRED) for every non-WASM
# build, to give the extension installer HTTPS. There is no upstream option to
# turn it off, so the engine binary links OpenSSL whether or not it ever
# downloads an extension.
#
# By default it links dynamically, which is the conventional choice: OpenSSL
# CVEs then arrive through the platform's package updates rather than requiring
# a rebuild of this component. The cost is that the binary is no longer fully
# self-contained -- it needs libssl/libcrypto 3 present at runtime, and on
# macOS the Homebrew path is baked in as an absolute path.
#
# Turn this ON to link OpenSSL statically and get a single self-contained
# artefact back. If you do, you own OpenSSL patching for this binary.
option(RE_OPENSSL_STATIC "Link OpenSSL statically into the engine" OFF)
if(RE_OPENSSL_STATIC)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    message(STATUS "rendering-engine: linking OpenSSL statically")
endif()

if(RE_LADYBUG_SOURCE_DIR)
    message(STATUS "rendering-engine: using local Ladybug at ${RE_LADYBUG_SOURCE_DIR}")
    if(NOT EXISTS "${RE_LADYBUG_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "RE_LADYBUG_SOURCE_DIR is set to '${RE_LADYBUG_SOURCE_DIR}' but no "
            "CMakeLists.txt was found there.")
    endif()
    add_subdirectory("${RE_LADYBUG_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_ladybug" EXCLUDE_FROM_ALL)
else()
    message(STATUS "rendering-engine: fetching Ladybug ${RE_LADYBUG_GIT_TAG}")
    FetchContent_Declare(ladybug
        GIT_REPOSITORY "${RE_LADYBUG_GIT_REPOSITORY}"
        GIT_TAG        "${RE_LADYBUG_GIT_TAG}"
        GIT_SHALLOW    TRUE
        # Every Ladybug submodule is an optional add-on (extensions, datasets,
        # language bindings). None are needed to build the core library.
        GIT_SUBMODULES ""
        GIT_PROGRESS   TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(ladybug)
endif()

if(NOT TARGET lbug)
    message(FATAL_ERROR
        "Ladybug was configured but the 'lbug' target does not exist. The "
        "upstream target layout may have changed; check src/CMakeLists.txt in "
        "the Ladybug source tree.")
endif()

# Treat Ladybug's headers as system headers. Without this our own warning set
# (ProjectWarnings.cmake -- -Wold-style-cast, -Wconversion, -Wcast-align and
# friends) fires on every Ladybug header we include, burying our diagnostics
# under ~130 warnings we cannot act on.
set_target_properties(lbug PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
        "$<TARGET_PROPERTY:lbug,INTERFACE_INCLUDE_DIRECTORIES>")

# Determining which Ladybug this actually is.
#
# Two things make this less obvious than it should be:
#
#  * Ladybug's own lbug::main::Version::getVersion() reports
#    LBUG_CMAKE_VERSION, which upstream defines as ${CMAKE_PROJECT_VERSION}.
#    In a subproject build that resolves to *rendering-engine's* version,
#    not Ladybug's. It is wrong here, so we do not use it.
#  * project() sets <PROJECT-NAME>_VERSION in the subproject's own directory
#    scope, so Lbug_VERSION is not visible to us either.
#
# Read it from the declaration itself, which is true for both the fetched and
# the local-checkout path.
if(RE_LADYBUG_SOURCE_DIR)
    set(RE_LADYBUG_TREE "${RE_LADYBUG_SOURCE_DIR}")
else()
    set(RE_LADYBUG_TREE "${ladybug_SOURCE_DIR}")
endif()

set(Lbug_VERSION "unknown")
if(EXISTS "${RE_LADYBUG_TREE}/CMakeLists.txt")
    file(READ "${RE_LADYBUG_TREE}/CMakeLists.txt" RE_LADYBUG_CMAKELISTS)
    string(REGEX MATCH "project\\(Lbug[ \t]+VERSION[ \t]+([0-9]+\\.[0-9]+\\.[0-9]+)"
        RE_LADYBUG_VERSION_MATCH "${RE_LADYBUG_CMAKELISTS}")
    if(CMAKE_MATCH_1)
        set(Lbug_VERSION "${CMAKE_MATCH_1}")
    endif()
    unset(RE_LADYBUG_CMAKELISTS)
endif()

if(Lbug_VERSION STREQUAL "unknown")
    message(WARNING
        "Could not determine the Ladybug version from ${RE_LADYBUG_TREE}. The "
        "binary will report 'unknown'. Upstream's project() declaration may "
        "have changed shape.")
endif()
message(STATUS "rendering-engine: Ladybug ${Lbug_VERSION}")

# Static, not lbug_shared: a single artefact is markedly simpler to ship to a
# Graviton host. Static linking here does not by itself make the binary fully
# self-contained -- see the OpenSSL note above for the remaining runtime
# dependency. `lbug` already exports its own include directories and link
# dependencies.
add_library(rainbohz::ladybug ALIAS lbug)

# ---------------------------------------------------------------------------
# CLI11
# ---------------------------------------------------------------------------
#
# Command line parsing for the engine binary. Header-only and C++20-clean, so
# it costs a compile of one translation unit and nothing at link time.
#
# Pinned from source like everything else (ADR 0001): a system package would
# break cross-compilation, which is the whole reason this project builds its
# dependencies itself.

set(RE_CLI11_GIT_REPOSITORY "https://github.com/CLIUtils/CLI11"
    CACHE STRING "CLI11 source repository")
set(RE_CLI11_GIT_TAG "v2.7.2" CACHE STRING "CLI11 version to build against")

# CLI11 builds its own tests, examples and a single-header generation step by
# default. None of that is wanted in a consumer build.
set(CLI11_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(CLI11_SINGLE_FILE    OFF CACHE BOOL "" FORCE)
set(CLI11_INSTALL        OFF CACHE BOOL "" FORCE)
set(CLI11_PRECOMPILED    OFF CACHE BOOL "" FORCE)

message(STATUS "rendering-engine: fetching CLI11 ${RE_CLI11_GIT_TAG}")
FetchContent_Declare(cli11
    GIT_REPOSITORY "${RE_CLI11_GIT_REPOSITORY}"
    GIT_TAG        "${RE_CLI11_GIT_TAG}"
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

# As for Ladybug: our warning set (-Wold-style-cast, -Wconversion and friends)
# is aimed at code we own, and CLI11's headers are not that.
get_target_property(RE_CLI11_INCLUDES CLI11 INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(CLI11 PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${RE_CLI11_INCLUDES}")

add_library(rainbohz::cli11 ALIAS CLI11)

# ---------------------------------------------------------------------------
# yaml-cpp
# ---------------------------------------------------------------------------
#
# Reads the render request that batch mode takes with --request (docs/design.md
# section 3.2). Chosen over rapidyaml because it is what the proof of concept's
# renderer used, and because every node carries its source line, which is what
# lets a bad request be reported against the line that is wrong.
#
# Pinned from source like everything else (ADR 0001). Upstream's tags are
# irregular -- `yaml-cpp-0.7.0`, then `0.8.0`, then `yaml-cpp-0.9.0` -- so check
# for newer releases with:
#
#   git ls-remote --tags --refs https://github.com/jbeder/yaml-cpp

set(RE_YAML_CPP_GIT_REPOSITORY "https://github.com/jbeder/yaml-cpp"
    CACHE STRING "yaml-cpp source repository")
set(RE_YAML_CPP_GIT_TAG "yaml-cpp-0.9.0" CACHE STRING "yaml-cpp version to build against")

# The library only, static, for the same single-artefact reason as Ladybug.
set(YAML_CPP_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS   OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL       OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

message(STATUS "rendering-engine: fetching yaml-cpp ${RE_YAML_CPP_GIT_TAG}")
FetchContent_Declare(yaml_cpp
    GIT_REPOSITORY "${RE_YAML_CPP_GIT_REPOSITORY}"
    GIT_TAG        "${RE_YAML_CPP_GIT_TAG}"
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(yaml_cpp)

if(NOT TARGET yaml-cpp::yaml-cpp)
    message(FATAL_ERROR
        "yaml-cpp was configured but the 'yaml-cpp::yaml-cpp' target does not "
        "exist. The upstream target layout may have changed.")
endif()

# As for Ladybug and CLI11: our warning set is for code we own.
get_target_property(RE_YAML_CPP_INCLUDES yaml-cpp INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(yaml-cpp PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${RE_YAML_CPP_INCLUDES}")

add_library(rainbohz::yaml_cpp ALIAS yaml-cpp)

# ---------------------------------------------------------------------------
# dr_wav
# ---------------------------------------------------------------------------
#
# Writes the rendered frames as 32-bit float WAV files (docs/rendering.md
# section 5). One C header from mackron/dr_libs, public domain or MIT-0 at the
# user's choice; chosen over a hand-written writer, libsndfile and AudioFile in
# ADR 0009.
#
# dr_libs does not tag releases per header, so the pin is a commit hash. To
# bump it, read the changelog at the top of dr_wav.h between the two commits,
# then run the WAV writer tests and the oracle, which both check the container
# rather than trust it.
#
# The repository holds the header only; there is no build system to run. It is
# exposed as an INTERFACE target whose include directory is SYSTEM, so that our
# warning set does not fire on code we do not own, and it is linked PRIVATE by
# src/wav_writer.cpp alone. No dr_wav type appears in a public header.

set(RE_DR_LIBS_GIT_REPOSITORY "https://github.com/mackron/dr_libs"
    CACHE STRING "dr_libs source repository")
set(RE_DR_LIBS_GIT_TAG "dfe8377631000664666519fdb83da193fd8037f4"
    CACHE STRING "dr_libs commit to build against")

message(STATUS "rendering-engine: fetching dr_libs ${RE_DR_LIBS_GIT_TAG}")
FetchContent_Declare(dr_libs
    GIT_REPOSITORY "${RE_DR_LIBS_GIT_REPOSITORY}"
    GIT_TAG        "${RE_DR_LIBS_GIT_TAG}"
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
# cpp-httplib
# ---------------------------------------------------------------------------
#
# The HTTP server of server mode (docs/server.md). One header, MIT; chosen over
# Boost.Beast and the full web frameworks in ADR 0016, because the server is
# three endpoints served one call at a time and needs nothing more.
#
# Pinned from source like everything else (ADR 0001). Check for newer
# releases with:
#
#   git ls-remote --tags --refs https://github.com/yhirose/cpp-httplib
#
# Upstream's CMakeLists is deliberately not run: by default it turns on HTTPS
# whenever it finds OpenSSL -- which Ladybug guarantees it will (see above) --
# and zlib, brotli and zstd compression likewise. The server speaks plain HTTP
# behind a gateway and wants none of them. SOURCE_SUBDIR names a directory
# with no CMakeLists.txt, so FetchContent only fetches, and the header is
# exposed as an INTERFACE target with none of CPPHTTPLIB_*_SUPPORT defined.
# It is linked PRIVATE by src/server.cpp's library alone; no httplib type
# appears in a public header.

set(RE_HTTPLIB_GIT_REPOSITORY "https://github.com/yhirose/cpp-httplib"
    CACHE STRING "cpp-httplib source repository")
set(RE_HTTPLIB_GIT_TAG "v0.58.0" CACHE STRING "cpp-httplib version to build against")

message(STATUS "rendering-engine: fetching cpp-httplib ${RE_HTTPLIB_GIT_TAG}")
FetchContent_Declare(httplib
    GIT_REPOSITORY "${RE_HTTPLIB_GIT_REPOSITORY}"
    GIT_TAG        "${RE_HTTPLIB_GIT_TAG}"
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

# httplib uses std::thread for its default task queue, which server mode
# replaces but still compiles, and sockets, which on Windows are Winsock.
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
#
# Reads and writes the JSON bodies of server mode. Header-only, MIT, and the
# library a C++ reader is most likely to know already.
#
# Pinned from source like everything else (ADR 0001). Check for newer
# releases with:
#
#   git ls-remote --tags --refs https://github.com/nlohmann/json
#
# Upstream's CMakeLists is well behaved as a subproject (its tests are off
# unless it is the top-level project), so it is used as it is.

set(RE_JSON_GIT_REPOSITORY "https://github.com/nlohmann/json"
    CACHE STRING "nlohmann/json source repository")
set(RE_JSON_GIT_TAG "v3.12.0" CACHE STRING "nlohmann/json version to build against")

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)

message(STATUS "rendering-engine: fetching nlohmann/json ${RE_JSON_GIT_TAG}")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY "${RE_JSON_GIT_REPOSITORY}"
    GIT_TAG        "${RE_JSON_GIT_TAG}"
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

# As for the others: our warning set is for code we own.
get_target_property(RE_JSON_INCLUDES nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(nlohmann_json PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${RE_JSON_INCLUDES}")

add_library(rainbohz::json ALIAS nlohmann_json)

# ---------------------------------------------------------------------------
# Highway
# ---------------------------------------------------------------------------
#
# Portable SIMD for the render kernel (src/render_kernel_simd.cpp, ADR 0027):
# NEON on Apple Silicon, SVE and SVE2 on Graviton, AVX2 and AVX-512 on x86,
# chosen at run time by Highway's dynamic dispatch. Only the core library is
# built; the vectorised sine and exponential are in the header-only
# hwy/contrib/math, so the rest of contrib (vqsort and friends, a long build)
# stays off. Apache-2.0 or BSD-3-Clause. Check for newer releases with:
#
#   git ls-remote --tags --refs https://github.com/google/highway

set(RE_HIGHWAY_GIT_REPOSITORY "https://github.com/google/highway"
    CACHE STRING "Highway source repository")
set(RE_HIGHWAY_GIT_TAG "1.4.0" CACHE STRING "Highway version to build against")

set(HWY_ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_CONTRIB  OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
set(HWY_FORCE_STATIC_LIBS ON CACHE BOOL "" FORCE)

message(STATUS "rendering-engine: fetching Highway ${RE_HIGHWAY_GIT_TAG}")
FetchContent_Declare(highway
    GIT_REPOSITORY "${RE_HIGHWAY_GIT_REPOSITORY}"
    GIT_TAG        "${RE_HIGHWAY_GIT_TAG}"
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(highway)

if(NOT TARGET hwy)
    message(FATAL_ERROR
        "Highway was configured but the 'hwy' target does not exist. "
        "The upstream target layout may have changed.")
endif()

# As for the others: our warning set is for code we own.
get_target_property(RE_HIGHWAY_INCLUDES hwy INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(hwy PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${RE_HIGHWAY_INCLUDES}")

add_library(rainbohz::highway ALIAS hwy)

# ---------------------------------------------------------------------------
# Apache Arrow (Parquet)
# ---------------------------------------------------------------------------
#
# The native engine-db backend reads a score's Parquet tables itself, without
# Ladybug (src/native_table_loader.cpp, ADR 0028). Arrow's C++ library is the
# reference reader of what hzlc's pyarrow writes. Only what that needs is
# built: the core library and Parquet, with Snappy, pyarrow's default codec.
# Every other component and codec is off. Apache-2.0. Arrow appears in no
# public header: only the loader includes it.
#
# Arrow is built as a separate CMake project (ExternalProject), not added to
# this one, because it builds its bundled Thrift and Snappy as targets of
# their own, and Ladybug already defines a target named `thrift` here. The two
# copies do not collide at link time: Ladybug's live in renamed namespaces
# (lbug_apache::thrift, lbug_snappy) and have no C API. Arrow is configured
# with this build's compiler, build type, flags (sanitizers included),
# interprocedural optimisation, toolchain file and compiler launcher, so that
# every preset links a consistent binary. Its bundled dependencies are
# fetched, by Arrow, at build time.
#
# Check for newer releases at https://arrow.apache.org/release/ and pin the
# archive by its SHA-256, checked against Apache's published .sha512 when it
# is bumped.

include(ExternalProject)

set(RE_ARROW_VERSION "22.0.0" CACHE STRING "Apache Arrow version to build against")
set(RE_ARROW_SHA256 "131250cd24dec0cddde04e2ad8c9e2bc43edc5e84203a81cf71cf1a33a6e7e0f"
    CACHE STRING "SHA-256 of the Apache Arrow source archive")

set(RE_ARROW_PREFIX "${CMAKE_BINARY_DIR}/_deps/arrow-install")
set(RE_ARROW_INCLUDE_DIR "${RE_ARROW_PREFIX}/include")
# Arrow names its static libraries with a `_static` suffix under MSVC, where
# a static and an import library would otherwise share a name. The Windows
# presets have not been built with Arrow yet; this follows Arrow's naming.
if(MSVC)
    set(re_arrow_static_suffix "_static")
else()
    set(re_arrow_static_suffix "")
endif()
set(re_arrow_lib "${RE_ARROW_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}")
set(RE_ARROW_PARQUET_LIB
    "${re_arrow_lib}parquet${re_arrow_static_suffix}${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(RE_ARROW_CORE_LIB "${re_arrow_lib}arrow${re_arrow_static_suffix}${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(RE_ARROW_BUNDLED_LIB
    "${re_arrow_lib}arrow_bundled_dependencies${CMAKE_STATIC_LIBRARY_SUFFIX}")

set(re_arrow_args
    -DCMAKE_INSTALL_PREFIX=${RE_ARROW_PREFIX}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
    -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=${CMAKE_INTERPROCEDURAL_OPTIMIZATION}
    -DARROW_BUILD_STATIC=ON
    -DARROW_BUILD_SHARED=OFF
    -DARROW_BUILD_TESTS=OFF
    -DARROW_BUILD_BENCHMARKS=OFF
    -DARROW_BUILD_EXAMPLES=OFF
    -DARROW_BUILD_UTILITIES=OFF
    -DARROW_PARQUET=ON
    -DARROW_WITH_SNAPPY=ON
    -DARROW_WITH_ZLIB=OFF
    -DARROW_WITH_ZSTD=OFF
    -DARROW_WITH_LZ4=OFF
    -DARROW_WITH_BROTLI=OFF
    -DARROW_WITH_BZ2=OFF
    -DARROW_WITH_RE2=OFF
    -DARROW_WITH_UTF8PROC=OFF
    -DARROW_WITH_OPENTELEMETRY=OFF
    -DARROW_COMPUTE=OFF
    -DARROW_CSV=OFF
    -DARROW_JSON=OFF
    -DARROW_DATASET=OFF
    -DARROW_FILESYSTEM=OFF
    -DARROW_FLIGHT=OFF
    -DARROW_GANDIVA=OFF
    -DARROW_ACERO=OFF
    -DARROW_SUBSTRAIT=OFF
    -DARROW_JEMALLOC=OFF
    -DARROW_MIMALLOC=OFF
    -DARROW_S3=OFF
    -DARROW_GCS=OFF
    -DARROW_AZURE=OFF
    -DARROW_HDFS=OFF
    -DARROW_ORC=OFF
    -DARROW_CUDA=OFF
    -DARROW_TESTING=OFF
    # The reader's speed is not what matters here, and plain code is the
    # same on every target this build has.
    -DARROW_SIMD_LEVEL=NONE
    -DARROW_RUNTIME_SIMD_LEVEL=NONE
    -DPARQUET_REQUIRE_ENCRYPTION=OFF
    -DPARQUET_BUILD_EXECUTABLES=OFF
    -DPARQUET_BUILD_EXAMPLES=OFF
    -DARROW_DEPENDENCY_SOURCE=BUNDLED
    -DARROW_DEPENDENCY_USE_SHARED=OFF
)
if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND re_arrow_args -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
endif()
if(CMAKE_OSX_SYSROOT)
    list(APPEND re_arrow_args -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT})
endif()
if(CMAKE_OSX_ARCHITECTURES)
    list(APPEND re_arrow_args -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES})
endif()
if(CMAKE_CXX_COMPILER_LAUNCHER)
    list(APPEND re_arrow_args
        -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
        -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER})
endif()
if(DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    list(APPEND re_arrow_args -DCMAKE_POLICY_VERSION_MINIMUM=${CMAKE_POLICY_VERSION_MINIMUM})
endif()

message(STATUS "rendering-engine: Apache Arrow ${RE_ARROW_VERSION}, built as an external project")
ExternalProject_Add(arrow_external
    URL         "https://archive.apache.org/dist/arrow/arrow-${RE_ARROW_VERSION}/apache-arrow-${RE_ARROW_VERSION}.tar.gz"
    URL_HASH    "SHA256=${RE_ARROW_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PREFIX      "${CMAKE_BINARY_DIR}/_deps/arrow"
    SOURCE_SUBDIR cpp
    CMAKE_ARGS  ${re_arrow_args}
    BUILD_BYPRODUCTS "${RE_ARROW_PARQUET_LIB}" "${RE_ARROW_CORE_LIB}" "${RE_ARROW_BUNDLED_LIB}"
    EXCLUDE_FROM_ALL TRUE
)

# The imported targets name the install directory, which must exist when
# CMake generates, before Arrow has been built into it.
file(MAKE_DIRECTORY "${RE_ARROW_INCLUDE_DIR}")

add_library(re_arrow_bundled STATIC IMPORTED GLOBAL)
set_target_properties(re_arrow_bundled PROPERTIES IMPORTED_LOCATION "${RE_ARROW_BUNDLED_LIB}")
add_dependencies(re_arrow_bundled arrow_external)

add_library(re_arrow_core STATIC IMPORTED GLOBAL)
set_target_properties(re_arrow_core PROPERTIES
    IMPORTED_LOCATION "${RE_ARROW_CORE_LIB}"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${RE_ARROW_INCLUDE_DIR}"
    INTERFACE_INCLUDE_DIRECTORIES "${RE_ARROW_INCLUDE_DIR}"
    # Arrow's static headers expect this, as its CMake package would set it.
    INTERFACE_COMPILE_DEFINITIONS "ARROW_STATIC"
    INTERFACE_LINK_LIBRARIES "re_arrow_bundled;Threads::Threads")
add_dependencies(re_arrow_core arrow_external)

add_library(re_parquet STATIC IMPORTED GLOBAL)
set_target_properties(re_parquet PROPERTIES
    IMPORTED_LOCATION "${RE_ARROW_PARQUET_LIB}"
    INTERFACE_COMPILE_DEFINITIONS "PARQUET_STATIC"
    INTERFACE_LINK_LIBRARIES "re_arrow_core")
add_dependencies(re_parquet arrow_external)

add_library(rainbohz::parquet ALIAS re_parquet)
