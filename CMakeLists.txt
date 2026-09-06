cmake_minimum_required(VERSION 3.20)
project(livesprite_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(livesprite STATIC
    src/ls_geometry.cpp
    src/ls_context.cpp
    src/ls_dependency.cpp
    src/ls_compile.cpp
    src/ls_serialize.cpp
    src/ls_instructions.cpp
    src/ls_json.cpp
)

target_include_directories(livesprite
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (MSVC)
    # No exceptions, no RTTI: the engine returns ls::Result<T> instead.
    target_compile_options(livesprite PRIVATE /W4 /WX)
else()
    target_compile_options(livesprite PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

option(LS_BUILD_TESTBED "Build the engine testbed gallery tool" ON)
if (LS_BUILD_TESTBED)
    add_executable(livesprite_testbed tools/ls_testbed.cpp)
    target_link_libraries(livesprite_testbed PRIVATE livesprite)
    target_include_directories(livesprite_testbed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
endif()

option(LS_BUILD_TESTS "Build engine tests" ON)
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

    add_executable(livesprite_plugin_tests tests/plugin_tests.cpp)
    target_link_libraries(livesprite_plugin_tests PRIVATE livesprite)
    add_test(NAME livesprite_plugin COMMAND livesprite_plugin_tests)
endif()

install(TARGETS livesprite
    EXPORT livesprite-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)
install(EXPORT livesprite-targets
    FILE LiveSpriteTargets.cmake
    NAMESPACE livesprite::
    DESTINATION lib/cmake/livesprite
)
