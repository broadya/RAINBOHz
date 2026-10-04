include(FetchContent)

# Same pin as rendering-engine/test/CMakeLists.txt.
set(SP_GOOGLETEST_GIT_TAG "v1.17.0" CACHE STRING "GoogleTest version")

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        "${SP_GOOGLETEST_GIT_TAG}"
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(spatial_poc_tests
    test_units.cpp
    test_spatial_gains.cpp
    test_oracle.cpp
)
target_link_libraries(spatial_poc_tests PRIVATE spatial_poc sp_warnings GTest::gtest_main)

# The golden data written by oracle/export_oracle.py from the Python package.
# Committed, so the tests need no Python.
target_compile_definitions(spatial_poc_tests PRIVATE
    SP_ORACLE_CSV="${CMAKE_CURRENT_SOURCE_DIR}/oracle/oracle_gains.csv")

gtest_discover_tests(spatial_poc_tests)
