cmake_minimum_required(VERSION 3.20)
# C is enabled so the C ABI can be tested by a C compiler rather than by C++
# pretending to be one. A header that only compiles as C++ is not a C header.
project(livesprite_engine VERSION 0.1.0 LANGUAGES CXX C)

# When LiveSprite is pulled into another project with add_subdirectory or
# FetchContent, that project wants the library and nothing else: not our tests,
# not our testbed, and definitely not our install rules writing into its prefix.
if (CMAKE_VERSION VERSION_LESS 3.21)
    get_property(LS_HAS_PARENT DIRECTORY PROPERTY PARENT_DIRECTORY)
    if (LS_HAS_PARENT)
        set(LS_TOP_LEVEL OFF)
    else()
        set(LS_TOP_LEVEL ON)
    endif()
else()
    set(LS_TOP_LEVEL ${PROJECT_IS_TOP_LEVEL})
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LS_SOURCES
    src/ls_geometry.cpp
    src/ls_context.cpp
    src/ls_dependency.cpp
    src/ls_compile.cpp
    src/ls_serialize.cpp
    src/ls_instructions.cpp
    src/ls_json.cpp
    src/ls_package.cpp
    src/ls_c_api.cpp
)

add_library(livesprite STATIC ${LS_SOURCES})

target_include_directories(livesprite
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# The spec calls for no RTTI, and for no exception to cross the API boundary.
# RTTI is switched off in the compiler; the boundary is held by returning
# ls::Result<T>, with the one operation that can genuinely fail at runtime
# (raster allocation) catching for itself.
# Determinism also depends on the floating-point contract. Left to their
# defaults, GCC and Clang will fuse a*b+c into an FMA, and MSVC contracts under
# its own rules; the same source then does different arithmetic per compiler and
# the golden hashes diverge. /fp:precise and -ffp-contract=off pin it. Fast-math
# is never acceptable here, at any optimisation level.
if (MSVC)
    # /permissive- turns on two-phase name lookup, so MSVC resolves templates
    # the way GCC and Clang do. Without it, a template calling an overload
    # declared later in the file compiles here and fails everywhere else --
    # which is exactly how the serializer reached CI.
    # /we4505 promotes "unreferenced function with internal linkage" to an
    # error, matching -Werror=unused-function on the other three.
    target_compile_options(livesprite PRIVATE
        /W4 /WX /GR- /fp:precise /permissive- /we4505)
else()
    target_compile_options(livesprite PRIVATE
        -Wall -Wextra -Wpedantic -Werror -fno-rtti -ffp-contract=off)
endif()

# Consumers write livesprite::livesprite whether they found an installed
# package or pulled the source in directly, so the two paths are interchangeable.
add_library(livesprite::livesprite ALIAS livesprite)

# The C ABI as a loadable shared library.
#
# The static library above is for C++ apps compiled alongside the engine. It is
# useless to Python, C#, Node or anything else that loads a library at runtime,
# and that is what this target is for: the same engine, exporting only the
# stable C surface, in a file a foreign function interface can open.
#
# Off by default because most builds here are the C++ one, and a shared library
# nobody loads is just a slower build.
option(LS_BUILD_SHARED_C "Build the C ABI as a shared library for language bindings" OFF)

if (LS_BUILD_SHARED_C)
    add_library(livesprite_c SHARED ${LS_SOURCES})
    add_library(livesprite::c ALIAS livesprite_c)

    target_include_directories(livesprite_c
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # LS_C_SHARED switches the header to dllimport/visibility attributes;
    # LS_C_BUILDING says this translation unit is the one exporting them.
    target_compile_definitions(livesprite_c PUBLIC LS_C_SHARED)
    target_compile_definitions(livesprite_c PRIVATE LS_C_BUILDING)

    # Everything except the C entry points stays hidden, so the C++ symbols are
    # not part of what a binding can accidentally reach for. The name stays
    # livesprite_c: a binding loads livesprite_c.dll / liblivesprite_c.so, and
    # it must not collide with the static library's import lib.
    set_target_properties(livesprite_c PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        POSITION_INDEPENDENT_CODE ON)

    if (MSVC)
        target_compile_options(livesprite_c PRIVATE
            /W4 /WX /GR- /fp:precise /permissive- /we4505)
    else()
        target_compile_options(livesprite_c PRIVATE
            -Wall -Wextra -Wpedantic -Werror -fno-rtti -ffp-contract=off)
    endif()
endif()

option(LS_BUILD_TESTBED "Build the engine testbed gallery tool" ${LS_TOP_LEVEL})
if (LS_BUILD_TESTBED)
    add_executable(livesprite_testbed tools/ls_testbed.cpp)
    target_link_libraries(livesprite_testbed PRIVATE livesprite)
    target_include_directories(livesprite_testbed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools)

    add_executable(livesprite_bench tools/ls_bench.cpp)
    target_link_libraries(livesprite_bench PRIVATE livesprite)

    # The worked example the documentation quotes. Built and run with the tests
    # so the documentation cannot drift from code that no longer compiles.
    add_executable(livesprite_example examples/first_sprite.cpp)
    target_link_libraries(livesprite_example PRIVATE livesprite)
    add_test(NAME livesprite_example COMMAND livesprite_example)
endif()

option(LS_BUILD_TESTS "Build engine tests" ${LS_TOP_LEVEL})
if (LS_BUILD_TESTS)
    enable_testing()

    add_executable(livesprite_geometry_tests tests/geometry_tests.cpp)
    target_link_libraries(livesprite_geometry_tests PRIVATE livesprite)
    add_test(NAME livesprite_geometry COMMAND livesprite_geometry_tests)

    add_executable(livesprite_context_tests tests/context_tests.cpp)
    target_link_libraries(livesprite_context_tests PRIVATE livesprite)
    add_test(NAME livesprite_context COMMAND livesprite_context_tests)

    add_executable(livesprite_compile_tests tests/compile_tests.cpp)
    target_link_libraries(livesprite_compile_tests PRIVATE livesprite)
    add_test(NAME livesprite_compile COMMAND livesprite_compile_tests)

    add_executable(livesprite_serialize_tests tests/serialize_tests.cpp)
    target_link_libraries(livesprite_serialize_tests PRIVATE livesprite)
    add_test(NAME livesprite_serialize COMMAND livesprite_serialize_tests)

    add_executable(livesprite_dither_tests tests/dither_tests.cpp)
    target_link_libraries(livesprite_dither_tests PRIVATE livesprite)
    add_test(NAME livesprite_dither COMMAND livesprite_dither_tests)

    add_executable(livesprite_anchor_tests tests/anchor_tests.cpp)
    target_link_libraries(livesprite_anchor_tests PRIVATE livesprite)
    add_test(NAME livesprite_anchor COMMAND livesprite_anchor_tests)

    add_executable(livesprite_instruction_tests tests/instruction_tests.cpp)
    target_link_libraries(livesprite_instruction_tests PRIVATE livesprite)
    add_test(NAME livesprite_instruction COMMAND livesprite_instruction_tests)

    add_executable(livesprite_render_tests tests/render_tests.cpp)
    target_link_libraries(livesprite_render_tests PRIVATE livesprite)
    add_test(NAME livesprite_render COMMAND livesprite_render_tests)

    add_executable(livesprite_editing_tests tests/editing_tests.cpp)
    target_link_libraries(livesprite_editing_tests PRIVATE livesprite)
    add_test(NAME livesprite_editing COMMAND livesprite_editing_tests)

    add_executable(livesprite_package_tests tests/package_tests.cpp)
    target_link_libraries(livesprite_package_tests PRIVATE livesprite)
    add_test(NAME livesprite_package COMMAND livesprite_package_tests)

    add_executable(livesprite_palette_tests tests/palette_tests.cpp)
    target_link_libraries(livesprite_palette_tests PRIVATE livesprite)
    add_test(NAME livesprite_palette COMMAND livesprite_palette_tests)

    add_executable(livesprite_outline_tests tests/outline_tests.cpp)
    target_link_libraries(livesprite_outline_tests PRIVATE livesprite)
    add_test(NAME livesprite_outline COMMAND livesprite_outline_tests)

    add_executable(livesprite_frame_tests tests/frame_tests.cpp)
    target_link_libraries(livesprite_frame_tests PRIVATE livesprite)
    add_test(NAME livesprite_frame COMMAND livesprite_frame_tests)

    # The determinism suite reaches into src/ for the maths kernels it checks.
    add_executable(livesprite_determinism_tests tests/determinism_tests.cpp)
    target_link_libraries(livesprite_determinism_tests PRIVATE livesprite)
    target_include_directories(livesprite_determinism_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME livesprite_determinism COMMAND livesprite_determinism_tests)

    # Compiled as C, on purpose: see the comment at the top of the file.
    add_executable(livesprite_c_api_tests tests/c_api_tests.c)
    target_link_libraries(livesprite_c_api_tests PRIVATE livesprite)
    set_target_properties(livesprite_c_api_tests PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
    if (MSVC)
        target_compile_options(livesprite_c_api_tests PRIVATE /W4 /WX)
    else()
        target_compile_options(livesprite_c_api_tests PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME livesprite_c_api COMMAND livesprite_c_api_tests)

    add_executable(livesprite_limit_tests tests/limit_tests.cpp)
    target_link_libraries(livesprite_limit_tests PRIVATE livesprite)
    add_test(NAME livesprite_limits COMMAND livesprite_limit_tests)

    add_executable(livesprite_plugin_tests tests/plugin_tests.cpp)
    target_link_libraries(livesprite_plugin_tests PRIVATE livesprite)
    add_test(NAME livesprite_plugin COMMAND livesprite_plugin_tests)
endif()

# ---------------------------------------------------------------- install ----
#
# Two supported ways to consume the engine from C++:
#
#   find_package(LiveSprite REQUIRED)            # against an installed copy
#   FetchContent_MakeAvailable(livesprite)       # against the source
#
# then in both cases:
#
#   target_link_libraries(my_app PRIVATE livesprite::livesprite)

option(LS_INSTALL "Generate the LiveSprite install and package rules" ${LS_TOP_LEVEL})

if (LS_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS livesprite
        EXPORT livesprite-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT livesprite-targets
        FILE LiveSpriteTargets.cmake
        NAMESPACE livesprite::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LiveSprite
    )

    # SameMajorVersion while the major is 0 still means exact-minor matching,
    # which is the honest contract before 1.0: the API is not settled yet.
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/LiveSpriteConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/LiveSpriteConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/LiveSpriteConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LiveSprite
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/LiveSpriteConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/LiveSpriteConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LiveSprite
    )
endif()
