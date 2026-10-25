# SPDX-License-Identifier: Apache-2.0

# Tests for rendering-engine.
#
# GoogleTest, fetched and pinned like every other dependency. Ladybug's own
# test suite is not built (see cmake/Dependencies.cmake); this is only for
# rendering-engine's code.

include(FetchContent)

set(RE_GOOGLETEST_GIT_TAG "v1.17.0" CACHE STRING "GoogleTest version")

# Required on Windows so that gtest links against the same CRT as everything
# else. Harmless elsewhere.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        "${RE_GOOGLETEST_GIT_TAG}"
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(rendering_engine_tests
    test_amplitude_maths.cpp
    test_cypher_script.cpp
    test_engine_db.cpp
    test_file_uri.cpp
    test_frame_plan.cpp
    test_integrity.cpp
    test_native_model.cpp
    test_native_table_loader.cpp
    test_options.cpp
    test_parallel.cpp
    test_precomputation.cpp
    test_render_kernel.cpp
    test_render_request.cpp
    test_rendering.cpp
    test_script_loader.cpp
    test_segment_maths.cpp
    test_server.cpp
    test_spatial_gains.cpp
    test_spatial_gains_oracle.cpp
    test_spatial_maths.cpp
    test_spatialise.cpp
    test_table_loader.cpp
    test_units.cpp
    test_wav_writer.cpp
    test_write_batch.cpp
    transform/test_musical_units.cpp
    transform/test_polytempo.cpp
    transform/test_squash.cpp
    transform/test_store_equivalence.cpp
    transform/test_tempo_map.cpp
    transform/test_timing_query.cpp
    transform/test_transform.cpp
)

# dr_wav is linked so that test_wav_writer can read a written file back with
# the same library. The implementation lives in the engine library
# (src/wav_writer.cpp); the test includes the header for its declarations only.
# nlohmann/json is linked so that test_server can read the server's responses.
target_link_libraries(rendering_engine_tests
    PRIVATE
        rainbohz::rendering_engine_lib
        rainbohz::dr_wav
        rainbohz::json
        rainbohz::project_warnings
        GTest::gtest_main
)

# The real engine-db schema and the real example score, so that the loader
# tests can prove they load in process at the pinned Ladybug version. That is a
# different claim from `make validate-schemas`, which exercises the Homebrew
# `lbug` CLI and a possibly different Ladybug build.
#
# These are the same two files a default `make run-rendering-engine` applies,
# deliberately: the tests exercise the path a developer actually runs.
target_compile_definitions(rendering_engine_tests
    PRIVATE
        RE_TEST_SCHEMA_FILE="${RE_DEFAULT_SCHEMA_FILE}"
        RE_TEST_EXAMPLE_STATE="${RE_EXAMPLE_STATE_FILE}"
        RE_TEST_TWO_SETS_STATE="${RE_TWO_SETS_STATE_FILE}"
        RE_TEST_TEMPO_MAP_STATE="${RE_TEMPO_MAP_STATE_FILE}"
        RE_TEST_OFFSET_COLLISION_STATE="${RE_OFFSET_COLLISION_STATE_FILE}"
        RE_TEST_SPATIAL_STATE="${RE_SPATIAL_STATE_FILE}"
        RE_TEST_SPATIAL_MUSICAL_STATE="${RE_SPATIAL_MUSICAL_STATE_FILE}"
        RE_SPATIAL_ORACLE_CSV="${CMAKE_CURRENT_SOURCE_DIR}/oracle/spatial_gains.csv"
        RE_TEST_COMPILED_SCORES="${CMAKE_CURRENT_BINARY_DIR}/compiled-scores"
)

# hzl-compiler's example scores, compiled to tables once per CTest run, for
# the tests that load a score into both engine-db backends (ADR 0028). The
# interpreter is the root .venv's, where hzlc is installed (CMakeLists.txt).
# Run directly, outside CTest, those tests fail and say how to get the
# scores.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME scores.compile
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/compile_scores.py"
        --examples "${CMAKE_CURRENT_SOURCE_DIR}/../../hzl-compiler/examples"
        --out-dir "${CMAKE_CURRENT_BINARY_DIR}/compiled-scores")
set_tests_properties(scores.compile PROPERTIES FIXTURES_SETUP compiled_scores)

gtest_discover_tests(rendering_engine_tests
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 120
    PROPERTIES FIXTURES_REQUIRED compiled_scores
)

# ---------------------------------------------------------------------------
# The oracle (docs/rendering.md section 11.1)
# ---------------------------------------------------------------------------
#
# An independent NumPy implementation of the rendering model checks the
# engine's output frame by frame. One CTest per example score and kernel
# (ADR 0027): the driver runs the engine on the score and its request with
# that kernel, then the oracle on every file written. A missing package fails
# the test, never skips it.

# The interpreter is chosen at the top level (CMakeLists.txt): the root .venv.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(RE_ORACLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/oracle")
set(RE_EXAMPLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../engine-db/examples")
set(RE_REQUESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../examples")

function(re_add_oracle_test name score request)
    foreach(kernel IN ITEMS scalar simd)
        add_test(NAME "oracle.${name}.${kernel}"
            COMMAND "${Python3_EXECUTABLE}" "${RE_ORACLE_DIR}/run_oracle_check.py"
                --engine "$<TARGET_FILE:rendering-engine>"
                --schema "${RE_DEFAULT_SCHEMA_FILE}"
                --state "${RE_EXAMPLES_DIR}/${score}.cypher"
                --request "${RE_REQUESTS_DIR}/${request}"
                --score "${RE_EXAMPLES_DIR}/${score}.json"
                --render-kernel "${kernel}"
                --out-dir "${CMAKE_CURRENT_BINARY_DIR}/oracle/${name}-${kernel}")
    endforeach()
endfunction()

re_add_oracle_test(minimum-score minimum-score minimum-request.yaml)
re_add_oracle_test(two-partial-sets two-partial-sets two-partial-sets-request.yaml)
re_add_oracle_test(tempo-map-score tempo-map-score tempo-map-request.yaml)
re_add_oracle_test(tempo-map-position-score tempo-map-position-score tempo-map-position-request.yaml)
re_add_oracle_test(offset-collision-score offset-collision-score offset-collision-request.yaml)
re_add_oracle_test(phase-target-score phase-target-score phase-target-request.yaml)
re_add_oracle_test(phase-musical-score phase-musical-score phase-musical-request.yaml)
re_add_oracle_test(polytempo-shift-score polytempo-shift-score polytempo-shift-request.yaml)
re_add_oracle_test(polytempo-stretch-score polytempo-stretch-score polytempo-stretch-request.yaml)

# The oracle on the native backend (ADR 0028): the hzl-compiler twins of
# three examples, compiled to tables by the scores.compile fixture and so
# loaded natively, checked against the Cypher examples' own oracle scores.
function(re_add_native_oracle_test example request)
    add_test(NAME "oracle.native.${example}"
        COMMAND "${Python3_EXECUTABLE}" "${RE_ORACLE_DIR}/run_oracle_check.py"
            --engine "$<TARGET_FILE:rendering-engine>"
            --schema "${RE_DEFAULT_SCHEMA_FILE}"
            --state "${CMAKE_CURRENT_BINARY_DIR}/compiled-scores/${example}/manifest.json"
            --request "${RE_REQUESTS_DIR}/${request}"
            --score "${RE_EXAMPLES_DIR}/${example}.json"
            --out-dir "${CMAKE_CURRENT_BINARY_DIR}/oracle/native-${example}")
    set_tests_properties("oracle.native.${example}" PROPERTIES FIXTURES_REQUIRED compiled_scores)
endfunction()

re_add_native_oracle_test(tempo-map-score tempo-map-request.yaml)
re_add_native_oracle_test(polytempo-shift-score polytempo-shift-request.yaml)
re_add_native_oracle_test(polytempo-stretch-score polytempo-stretch-request.yaml)

# Server mode, end to end over HTTP (docs/server.md): the same score and
# request as an oracle test, served instead of run as a batch, and the
# files the server names checked by the same oracle. One score in musical
# time and one in physical time, so both paths through the transform are
# served. Every response is checked against the OpenAPI document. For a
# score in musical time, the optional last argument lists the beats of the
# first partial's frequency envelope nodes, and the tempo-map calls are
# checked against the oracle's times.
set(RE_OPENAPI_SPEC "${CMAKE_CURRENT_SOURCE_DIR}/../api/openapi.yaml")
function(re_add_server_test name score request)
    set(beats_args "")
    if(ARGC GREATER 3)
        set(beats_args --beats "${ARGV3}")
    endif()
    add_test(NAME "server.${name}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/server/run_server_check.py"
            --engine "$<TARGET_FILE:rendering-engine>"
            --schema "${RE_DEFAULT_SCHEMA_FILE}"
            --state "${RE_EXAMPLES_DIR}/${score}.cypher"
            --request "${RE_REQUESTS_DIR}/${request}"
            --score "${RE_EXAMPLES_DIR}/${score}.json"
            --out-dir "${CMAKE_CURRENT_BINARY_DIR}/server/${name}"
            --spec "${RE_OPENAPI_SPEC}"
            ${beats_args})
    set_tests_properties("server.${name}" PROPERTIES TIMEOUT 300)
endfunction()

re_add_server_test(tempo-map-score tempo-map-score tempo-map-request.yaml "0,2,4,8")

# The same calls on the native backend (ADR 0028): the hzl-compiler twin of
# an example, compiled to tables by the scores.compile fixture, which the
# server loads natively because the score is a table manifest. The oracle is
# the Cypher example's, which the twin must match (make test-hzl-oracle).
set(RE_COMPILED_SCORES_DIR "${CMAKE_CURRENT_BINARY_DIR}/compiled-scores")
function(re_add_native_server_test name example request)
    set(beats_args "")
    if(ARGC GREATER 3)
        set(beats_args --beats "${ARGV3}")
    endif()
    add_test(NAME "server.native.${name}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/server/run_server_check.py"
            --engine "$<TARGET_FILE:rendering-engine>"
            --schema "${RE_DEFAULT_SCHEMA_FILE}"
            --state "${RE_COMPILED_SCORES_DIR}/${example}/manifest.json"
            --request "${RE_REQUESTS_DIR}/${request}"
            --score "${RE_EXAMPLES_DIR}/${example}.json"
            --out-dir "${CMAKE_CURRENT_BINARY_DIR}/server/native-${name}"
            --spec "${RE_OPENAPI_SPEC}"
            ${beats_args})
    set_tests_properties("server.native.${name}" PROPERTIES
        TIMEOUT 300 FIXTURES_REQUIRED compiled_scores)
endfunction()

re_add_native_server_test(tempo-map-score tempo-map-score tempo-map-request.yaml "0,2,4,8")
re_add_server_test(two-partial-sets two-partial-sets two-partial-sets-request.yaml)

# The OpenAPI document (api/openapi.yaml): valid OpenAPI 3.1.1, with every
# example fitting its schema.
add_test(NAME "server.openapi"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/server/openapi_check.py"
        --spec "${RE_OPENAPI_SPEC}")
