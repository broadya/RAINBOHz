# SPDX-License-Identifier: Apache-2.0

# Tests for playback-client.
#
# GoogleTest, pinned as rendering-engine's (rendering-engine/test/CMakeLists.txt).
# The unit tests drive the threads with a scripted fake API and a memory sink;
# no server and no audio device are needed. The end-to-end test runs the real
# binary against a real rendering-engine server.

include(FetchContent)

set(PC_GOOGLETEST_GIT_TAG "v1.17.0" CACHE STRING "GoogleTest version")

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        "${PC_GOOGLETEST_GIT_TAG}"
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(playback_client_tests
    test_files.cpp
    test_lane_board.cpp
    test_session.cpp
    test_units_and_parsing.cpp
)

target_link_libraries(playback_client_tests
    PRIVATE
        rainbohz::playback_client_lib
        rainbohz::project_warnings
        GTest::gtest_main
)

gtest_discover_tests(playback_client_tests
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 60
)

# ---------------------------------------------------------------------------
# End to end (docs/design.md §8)
# ---------------------------------------------------------------------------
#
# Starts rendering-engine in server mode, loads an example score, runs
# playback-client with --output-wav, and checks the file against the frames
# the server rendered, sample for sample. Skipped (77) when the engine has not
# been built.
#
# The engine is taken from rendering-engine's build tree for the same preset,
# which is this build tree's name. Override with -DPC_RENDERING_ENGINE=...

get_filename_component(PC_PRESET_NAME "${CMAKE_BINARY_DIR}" NAME)
cmake_path(SET PC_RENDERING_ENGINE_DEFAULT NORMALIZE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../rendering-engine/build/${PC_PRESET_NAME}/bin/rendering-engine")
set(PC_RENDERING_ENGINE "${PC_RENDERING_ENGINE_DEFAULT}"
    CACHE FILEPATH "The rendering-engine binary the end-to-end test serves with")
cmake_path(SET PC_ENGINE_SCHEMA NORMALIZE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../engine-db/schema/engine-schema.cypher")
cmake_path(SET PC_EXAMPLE_SCORE NORMALIZE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../engine-db/examples/tempo-map-score.cypher")

# The interpreter is chosen at the top level (CMakeLists.txt): the root .venv.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME "playback.e2e"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_playback_check.py"
        --client "$<TARGET_FILE:playback-client>"
        --engine "${PC_RENDERING_ENGINE}"
        --schema "${PC_ENGINE_SCHEMA}"
        --score "${PC_EXAMPLE_SCORE}"
        --work-dir "${CMAKE_CURRENT_BINARY_DIR}/e2e")
set_tests_properties("playback.e2e" PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
