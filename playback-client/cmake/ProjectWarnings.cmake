# Warning set for playback-client's own code.
#
# Applied as an INTERFACE target and linked PRIVATE, so it never leaks into
# dependencies. miniaudio, cpp-httplib and the other third-party code keep their own
# warning settings; forcing this set on them would be unbuildable noise.
#
# Both a GCC/Clang set and an MSVC set are defined, because the component must
# build on Linux/GCC and Windows/MSVC as well as on clang.

add_library(project_warnings INTERFACE)
add_library(rainbohz::project_warnings ALIAS project_warnings)

set(PC_CLANG_GCC_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow                # a shadowed variable in phase arithmetic is a silent wrong answer
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion            # narrowing in sample/time arithmetic must be explicit
    -Wsign-conversion
    -Wdouble-promotion      # accidental float->double in hot paths
    -Wnull-dereference
    -Wformat=2
    -Wimplicit-fallthrough
)

set(PC_GCC_ONLY_WARNINGS
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wuseless-cast
)

# -Wdocumentation checks Doxygen comments against the code they document: a
# @param naming a parameter that does not exist, a @return on a void function.
# It catches comments gone stale after a rename. It does not report missing
# comments; Doxygen's own warnings do that (see Doxyfile).
set(PC_CLANG_ONLY_WARNINGS
    -Wdocumentation
)

set(PC_MSVC_WARNINGS
    /W4
    /permissive-            # standards conformance; MSVC's default is not
    /w14242 /w14254 /w14263 /w14265 /w14287
    /we4289
    /w14296 /w14311 /w14545 /w14546 /w14547 /w14549 /w14555
    /w14619 /w14640 /w14826 /w14905 /w14906 /w14928
)

if(MSVC)
    target_compile_options(project_warnings INTERFACE ${PC_MSVC_WARNINGS})
    if(PC_ENABLE_WERROR)
        target_compile_options(project_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(project_warnings INTERFACE ${PC_CLANG_GCC_WARNINGS})
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(project_warnings INTERFACE ${PC_GCC_ONLY_WARNINGS})
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(project_warnings INTERFACE ${PC_CLANG_ONLY_WARNINGS})
    endif()
    if(PC_ENABLE_WERROR)
        target_compile_options(project_warnings INTERFACE -Werror)
    endif()
endif()

unset(PC_CLANG_GCC_WARNINGS)
unset(PC_GCC_ONLY_WARNINGS)
unset(PC_CLANG_ONLY_WARNINGS)
unset(PC_MSVC_WARNINGS)
