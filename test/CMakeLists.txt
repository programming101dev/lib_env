cmake_minimum_required(VERSION 3.14)
project(p101_env_tests C)
enable_testing()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(P101_TEST_COVERAGE "Instrument the code under test for coverage" OFF)
set(P101_TEST_COVERAGE_FLAGS "")
if(P101_TEST_COVERAGE)
    set(P101_TEST_COVERAGE_FLAGS --coverage)
endif()

add_library(p101_env_under_test STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/env.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../lib_tool_event/src/event.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../lib_error/src/check.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../lib_error/src/error.c"
)
target_include_directories(p101_env_under_test PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../lib_tool_event/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../lib_error/include"
)
target_compile_definitions(p101_env_under_test PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(p101_env_under_test PRIVATE ${P101_TEST_COVERAGE_FLAGS})

find_package(Threads REQUIRED)

add_executable(test_env test_env.c)
target_link_libraries(test_env PRIVATE p101_env_under_test m Threads::Threads)
target_compile_definitions(test_env PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(test_env PRIVATE ${P101_TEST_COVERAGE_FLAGS})
target_link_options(test_env PRIVATE ${P101_TEST_COVERAGE_FLAGS})
add_test(NAME test_env COMMAND test_env)
