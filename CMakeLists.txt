cmake_minimum_required(VERSION 3.10)
project(p101_env
        VERSION 0.0.1
        DESCRIPTION ""
        LANGUAGES C)

message(STATUS "Compiler being used: ${CMAKE_C_COMPILER}")

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Source files for the library
set(SOURCES
        ${CMAKE_SOURCE_DIR}/src/env.c
)

# Define the list of header files to install
set(HEADER_LIST
        ${CMAKE_SOURCE_DIR}/include/p101_env/env.h
)

# Build the shared library
add_library(p101_env SHARED ${SOURCES})

target_include_directories(p101_env PRIVATE /usr/local/include)
target_link_directories(p101_env PRIVATE /usr/local/lib)
target_link_directories(p101_env PRIVATE /usr/local/lib64)

# Enable sanitizer checks
target_compile_options(p101_env PRIVATE -fsanitize=undefined -fsanitize=address)
target_link_libraries(p101_env PRIVATE -fsanitize=undefined -fsanitize=address)

# Include directories (if needed)
target_include_directories(p101_env PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Link libraries (if needed)
target_link_libraries(p101_env PRIVATE p101_error)

# Common compiler flags
set(STANDARD_FLAGS
    -Werror
)

# Initialize lists for compiler flags
set(COMMON_FLAGS ${STANDARD_FLAGS})

function(split_string_into_list _input_string _output_list)
    string(REGEX REPLACE "[ ]+" ";" _split_list "${_input_string}")
    set(${_output_list} ${_split_list} PARENT_SCOPE)
endfunction()

# Extract the compiler name without the path
get_filename_component(COMPILER_NAME "${CMAKE_C_COMPILER}" NAME)

# Import warning_flags.txt
file(STRINGS "../${COMPILER_NAME}_warning_flags.txt" WARNING_FLAGS_STRING)
split_string_into_list("${WARNING_FLAGS_STRING}" WARNING_FLAGS_LIST)

# Import analyzer_flags.txt
file(STRINGS "../${COMPILER_NAME}_analyzer_flags.txt" ANALYZER_FLAGS_STRING)
split_string_into_list("${WARNING_FLAGS_STRING}" ANALYZER_FLAGS_LIST)

# Import debug_flags.txt
file(STRINGS "../${COMPILER_NAME}_debug_flags.txt" DEBUG_FLAGSSTRING)
split_string_into_list("${DEBUG_FLAGS_STRING}" DEBUG_FLAGS_LIST)

# Import sanitizer_flags.txt
file(STRINGS "../${COMPILER_NAME}_sanitizer_flags.txt" SANITIZER_FLAGS_STRING)
split_string_into_list("${SANITIZER_FLAGS_STRING}" SANITIZER_FLAGS_LIST)

# Set compiler flags for the target
target_compile_options(p101_env PRIVATE
        ${COMMON_FLAGS}
        ${WARNING_FLAGS_LIST}
        ${ANALYZER_FLAGS_LIST}
        ${DEBUG_FLAGS_LIST}
        ${SANITIZER_FLAGS_LIST}
)

# Get compiler flags for the target
get_target_property(COMPILE_OPTIONS p101_env COMPILE_OPTIONS)
string(REPLACE ";" " " COMPILE_OPTIONS_STR "${COMPILE_OPTIONS}")
message(STATUS "COMPILE_OPTIONS: ${COMPILE_OPTIONS_STR}")

find_program(CLANG_FORMAT NAMES "clang-format" REQUIRED)
find_program(CLANG_TIDY NAMES "clang-tidy" REQUIRED)

# Format source files using clang-format
add_custom_target(format
        COMMAND ${CLANG_FORMAT} --style=file -i ${SOURCES} ${HEADER_LIST} ${HEADER_LIST_ARPA} ${HEADER_LIST_NET} ${HEADER_LIST_SYS}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-format on source files"
)

# Lint source files using clang-tidy
add_custom_target(lint
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMAND ${CLANG_TIDY} ${SOURCES} -quiet --warnings-as-errors='*' -checks=*,-llvmlibc-restrict-system-libc-headers,-altera-struct-pack-align,-readability-identifier-length,-altera-unroll-loops,-cppcoreguidelines-init-variables,-cert-err33-c,-modernize-macro-to-enum,-bugprone-easily-swappable-parameters,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-altera-id-dependent-backward-branch,-concurrency-mt-unsafe,-misc-unused-parameters,-hicpp-signed-bitwise,-google-readability-todo,-cert-msc30-c,-cert-msc50-cpp,-readability-function-cognitive-complexity,-clang-analyzer-security.insecureAPI.strcpy,-cert-env33-c,-android-cloexec-accept,-clang-analyzer-security.insecureAPI.rand,-misc-include-cleaner -- ${CMAKE_C_FLAGS} ${COMMON_FLAGS} -I${CMAKE_SOURCE_DIR}/include -I/usr/local/include
        COMMENT "Running clang-tidy"
)

add_dependencies(p101_env format)
add_dependencies(p101_env lint)

# Check if the system uses 64-bit libraries
get_property(LIB64 GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS)

# Set the library suffix based on whether it's 64-bit or not
if ("${LIB64}" STREQUAL "TRUE")
    set(LIBSUFFIX 64)
else()
    set(LIBSUFFIX "")
endif()

# Print the LIBSUFFIX variable for debugging
message("LIBSUFFIX: ${LIBSUFFIX}")

# Define the installation directory for libraries
set(INSTALL_LIB_DIR lib${LIBSUFFIX} CACHE PATH "Installation directory for libraries")
mark_as_advanced(INSTALL_LIB_DIR)

set_target_properties(p101_env PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})

# Install the library target
install(TARGETS p101_env
        LIBRARY DESTINATION ${INSTALL_LIB_DIR}
        COMPONENT library
        NAMELINK_COMPONENT development
)

# Install header files
install(FILES ${HEADER_LIST}
        DESTINATION include/p101_env
)
