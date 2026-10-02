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
    test_options.cpp
    test_precomputation.cpp
    test_render_kernel.cpp
    test_render_request.cpp
    test_rendering.cpp
    test_script_loader.cpp
    test_segment_maths.cpp
    test_server.cpp
    test_units.cpp
    test_wav_writer.cpp
    transform/test_musical_units.cpp
    transform/test_squash.cpp
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
)

gtest_discover_tests(rendering_engine_tests
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 120
)

# ---------------------------------------------------------------------------
# The oracle (docs/rendering.md section 11.1)
# ---------------------------------------------------------------------------
#
# An independent NumPy implementation of the rendering model checks the
# engine's output frame by frame. One CTest per example score: the driver
# runs the engine on the score and its request, then the oracle on every file
# written. Skipped with a notice (exit 77) when NumPy is not importable, so
# that the C++ suite does not depend on Python; not registered at all when no
# Python 3 interpreter is found at configure time.

# Which interpreter is found is decided at the top level (CMakeLists.txt,
# Python3_FIND_STRATEGY), because Ladybug's own build looks Python up first.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    set(RE_ORACLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/oracle")
    set(RE_EXAMPLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../engine-db/examples")
    set(RE_REQUESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../examples")

    function(re_add_oracle_test name score request)
        add_test(NAME "oracle.${name}"
            COMMAND "${Python3_EXECUTABLE}" "${RE_ORACLE_DIR}/run_oracle_check.py"
                --engine "$<TARGET_FILE:rendering-engine>"
                --schema "${RE_DEFAULT_SCHEMA_FILE}"
                --state "${RE_EXAMPLES_DIR}/${score}.cypher"
                --request "${RE_REQUESTS_DIR}/${request}"
                --score "${RE_EXAMPLES_DIR}/${score}.json"
                --out-dir "${CMAKE_CURRENT_BINARY_DIR}/oracle/${name}")
        set_tests_properties("oracle.${name}" PROPERTIES SKIP_RETURN_CODE 77)
    endfunction()

    re_add_oracle_test(minimum-score minimum-score minimum-request.yaml)
    re_add_oracle_test(two-partial-sets two-partial-sets two-partial-sets-request.yaml)
    re_add_oracle_test(tempo-map-score tempo-map-score tempo-map-request.yaml)
    re_add_oracle_test(offset-collision-score offset-collision-score offset-collision-request.yaml)

    # Server mode, end to end over HTTP (docs/server.md): the same score and
    # request as an oracle test, served instead of run as a batch, and the
    # files the server names checked by the same oracle. One score in musical
    # time and one in physical time, so both paths through the transform are
    # served. Every response is checked against the OpenAPI document where
    # PyYAML and jsonschema are installed. For a score in musical time, the
    # optional last argument lists the beats of the first partial's frequency
    # knots, and the tempo-map calls are checked against the oracle's times.
    # Skipped like the oracle when NumPy is missing.
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
        set_tests_properties("server.${name}" PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    endfunction()

    re_add_server_test(tempo-map-score tempo-map-score tempo-map-request.yaml "0,2,4,8")
    re_add_server_test(two-partial-sets two-partial-sets two-partial-sets-request.yaml)

    # The OpenAPI document (api/openapi.yaml): valid OpenAPI 3.1.1, with every
    # example fitting its schema. Skipped when openapi-spec-validator,
    # jsonschema or PyYAML is missing.
    add_test(NAME "server.openapi"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/server/openapi_check.py"
            --spec "${RE_OPENAPI_SPEC}")
    set_tests_properties("server.openapi" PROPERTIES SKIP_RETURN_CODE 77)
else()
    message(STATUS "rendering-engine: no Python 3 interpreter; the oracle and server tests are not registered")
endif()
