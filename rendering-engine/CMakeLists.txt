# 3.28 for FetchContent's EXCLUDE_FROM_ALL. Ubuntu 24.04, current Homebrew and
# current Visual Studio all ship at least this.
cmake_minimum_required(VERSION 3.28)

# rendering-engine
#
# The component identified as `rendering-engine` in architecture/overview.md.
# It hosts `engine-db` as an in-process Ladybug graph database and performs
# every operation that needs it: loading, transformation from the canonical
# model to the rendering model, precomputation, and rendering.
#
# Partly implemented. `batch` mode stands up engine-db with the schema applied
# from one Cypher file, loads an initial database state from a second, reads
# and checks a YAML render request, precomputes every partial's integrable
# segments and frame entries (docs/precomputation.md), and renders the
# requested frames to WAV files (docs/rendering.md). Before precomputation it
# transforms every partial set with a musical timeline into per-partial
# physical timelines through the score's tempo map (docs/transform.md).
# `server` mode runs the same steps as a long-running REST service, holding
# the score in memory between calls (docs/server.md). The `distributed` mode
# is recognised but not implemented.

project(rendering_engine
    VERSION 0.1.0
    DESCRIPTION "RAINBOHz rendering engine"
    LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Standard and global settings
# ---------------------------------------------------------------------------

# C++20, per README.md. Ladybug itself is C++20; going higher buys nothing until
# a C++23 feature is actually needed, and would narrow the set of usable
# compilers on the Linux and Windows targets.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emitted for clangd; the presets place it where the editor will find it.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Ladybug loads extensions via dlopen and needs its own RPATH to survive an
# install. Keep the build tree runnable without LD_LIBRARY_PATH fiddling.
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)

option(RE_BUILD_TESTS "Build rendering-engine tests" ON)
option(RE_ENABLE_WERROR "Treat warnings as errors in rendering-engine code" OFF)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "rendering-engine: ccache enabled (${CCACHE_PROGRAM})")
endif()

# Python is used only by the oracle tests (test/CMakeLists.txt), but Ladybug's
# build looks it up first, so the lookup strategy has to be set here to take
# effect. LOCATION rather than CMake's default VERSION: the interpreter first
# on PATH is the one a developer runs by hand and has installed NumPy into,
# where the newest interpreter on the machine may have nothing installed.
# On macOS, frameworks are searched before PATH unless told otherwise, and
# Homebrew installs Python as a framework. Override with
# -DPython3_EXECUTABLE=... if the one on PATH is not the right one either.
set(Python3_FIND_STRATEGY LOCATION)
set(Python3_FIND_FRAMEWORK LAST)

include(ProjectWarnings)
include(Dependencies)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

# The component's logic lives in a library so that tests can link it without
# going through main(). Sources are listed explicitly, never globbed --
# globbing hides added files from the configure step.
add_library(rendering_engine_lib STATIC
    src/cypher_script.cpp
    src/engine_db.cpp
    src/file_uri.cpp
    src/frame_plan.cpp
    src/integrity.cpp
    src/options.cpp
    src/precomputation.cpp
    src/render_kernel.cpp
    src/render_request.cpp
    src/rendering.cpp
    src/report.cpp
    src/score_pipeline.cpp
    src/script_loader.cpp
    src/server.cpp
    src/transform/squash.cpp
    src/transform/tempo_map.cpp
    src/transform/timing_query.cpp
    src/transform/transform.cpp
    src/wav_writer.cpp
)
add_library(rainbohz::rendering_engine_lib ALIAS rendering_engine_lib)

target_include_directories(rendering_engine_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(rendering_engine_lib
    PUBLIC  rainbohz::ladybug
    # CLI11 (src/options.cpp), yaml-cpp (src/render_request.cpp), dr_wav
    # (src/wav_writer.cpp), and cpp-httplib and nlohmann/json
    # (src/server.cpp) appear in no public header, so they stay out of the
    # interface -- the same rule Ladybug's own types are held to.
    PRIVATE rainbohz::cli11 rainbohz::yaml_cpp rainbohz::dr_wav rainbohz::httplib
            rainbohz::json rainbohz::project_warnings
)

# The schema the engine applies to an empty database when no --schema is given.
#
# This is the in-repo engine-db schema file, so that a developer build picks up
# a schema edit without a reconfigure, and so that the binary does not have to
# carry a copy of a file that is still under construction. It is a development
# affordance: a deployed binary is expected to pass --schema explicitly, since
# this absolute path will not exist on the target.
#
# One file, not a directory: the engine names the schema it applies. See
# docs/design.md section 3.1.
cmake_path(SET RE_DEFAULT_SCHEMA_FILE NORMALIZE
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine-db/schema/engine-schema.cypher")
if(NOT EXISTS "${RE_DEFAULT_SCHEMA_FILE}")
    message(WARNING
        "rendering-engine: no schema file at ${RE_DEFAULT_SCHEMA_FILE}. The "
        "binary will require an explicit --schema.")
endif()
message(STATUS "rendering-engine: default schema file ${RE_DEFAULT_SCHEMA_FILE}")

# The example score the tests load, and the file `make run-rendering-engine`
# passes as --state. The engine itself has no default state: a run names the
# score it loads.
cmake_path(SET RE_EXAMPLE_STATE_FILE NORMALIZE
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine-db/examples/minimum-score.cypher")

# The second example score: two partial sets, glides, fades, a mid-frame
# start and partials leaving the audio range. The rendering tests and the
# oracle exercise it.
cmake_path(SET RE_TWO_SETS_STATE_FILE NORMALIZE
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine-db/examples/two-partial-sets.cypher")

# Two scores in the canonical model, in musical time, which the transform
# converts before precomputation: one with a tempo ramp and per-property NULL
# states, one whose fixed offsets make points collide (docs/transform.md).
cmake_path(SET RE_TEMPO_MAP_STATE_FILE NORMALIZE
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine-db/examples/tempo-map-score.cypher")
cmake_path(SET RE_OFFSET_COLLISION_STATE_FILE NORMALIZE
    "${CMAKE_CURRENT_SOURCE_DIR}/../engine-db/examples/offset-collision-score.cypher")

# See cmake/Dependencies.cmake for why the Ladybug version is passed in rather
# than read back from the library at runtime.
target_compile_definitions(rendering_engine_lib
    PRIVATE
        RE_LADYBUG_VERSION="${Lbug_VERSION}"
        RE_DEFAULT_SCHEMA_FILE="${RE_DEFAULT_SCHEMA_FILE}"
)

add_executable(rendering-engine src/main.cpp)
target_link_libraries(rendering-engine
    PRIVATE rainbohz::rendering_engine_lib rainbohz::project_warnings
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(RE_BUILD_TESTS)
    include(CTest)
    add_subdirectory(test)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
install(TARGETS rendering-engine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
