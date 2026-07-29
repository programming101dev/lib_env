cmake_minimum_required(VERSION 3.14)
project(p101_env_fuzz C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Fuzzing requires clang with libFuzzer")
endif()

set(P101_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -g -O1)

add_executable(fuzz
        fuzz_env.c
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/env.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../lib_tool_event/src/event.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../lib_error/src/check.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../lib_error/src/error.c"
)
target_include_directories(fuzz PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../lib_tool_event/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../lib_error/include"
)
target_compile_definitions(fuzz PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(fuzz PRIVATE ${P101_FUZZ_FLAGS})
target_link_options(fuzz PRIVATE ${P101_FUZZ_FLAGS})
target_link_libraries(fuzz PRIVATE m)
