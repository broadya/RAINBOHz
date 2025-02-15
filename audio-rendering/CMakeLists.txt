cmake_minimum_required(VERSION 3.16)
project(RAINBOHzAudioRendering VERSION 0.1.0)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Add debug flags for LLDB
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0 -fno-omit-frame-pointer")

# Ensure the Debug build type is used
set(CMAKE_BUILD_TYPE Debug)

# Sanitize in debug mode
#if (CMAKE_BUILD_TYPE MATCHES "Debug")
#add_compile_options(
#    -Wall -Wextra -Wpedantic
#    -Wshadow=local -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
#    -Woverloaded-virtual -Wconversion -Wsign-conversion
#    -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wmisleading-indentation
#    )
#endif()

# Include directories (if you have headers in an include directory)
include_directories(include)

# Add source files for the main application
file(GLOB SOURCES "src/*.cpp")

# Create a static library with the core application code.
add_library(audio_rendering_lib STATIC
#    src/sine_wave_generator.cpp
    src/paxel_generator.cpp
    src/wav_writer.cpp
    src/physical_envelope_generator.cpp
    src/paxel_generator.cpp
    # Add more source files as needed...
)

# Create the application executable by linking with the library
add_executable(audio_rendering
    src/main.cpp
)

target_link_libraries(audio_rendering
    audio_rendering_lib
)

# Fetch and include Google Test for unit testing
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/release-1.11.0.zip
)
FetchContent_MakeAvailable(googletest)

# Enable testing
enable_testing()

# Add the test executable
add_executable(testSuite
#    tests/test_partial_generator.cpp
#    tests/test_multi_partial_generator.cpp
#    tests/test_physical_envelope_generator.cpp
    tests/test_paxel_generator.cpp
#    tests/test_audio_helpers.cpp
# Add more test files as needed...
)

# Link the test executable against Google Test
target_link_libraries(testSuite
    audio_rendering_lib
    gtest_main
)

# Discover tests with CTest
include(GoogleTest)
gtest_discover_tests(testSuite)

# Documentation
find_package(Doxygen)
if(DOXYGEN_FOUND)
    add_custom_target(doc
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM)
endif()
