cmake_minimum_required(VERSION 3.21)

# Load project configuration
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/config.cmake")
    message(FATAL_ERROR "Missing config.cmake file. This file is required to define project settings.")
endif()
include("${CMAKE_SOURCE_DIR}/config.cmake")

message(STATUS "Loaded config.cmake variables:")
message(STATUS "  PROJECT_NAME: ${PROJECT_NAME}")
message(STATUS "  PROJECT_VERSION: ${PROJECT_VERSION}")
message(STATUS "  PROJECT_DESCRIPTION: ${PROJECT_DESCRIPTION}")
message(STATUS "  EXECUTABLE_TARGETS: ${EXECUTABLE_TARGETS}")
message(STATUS "  LIBRARY_TARGETS: ${LIBRARY_TARGETS}")
message(STATUS "  STANDARD_FLAGS: ${STANDARD_FLAGS}")
message(STATUS "  SANITIZER_LIST: ${SANITIZER_LIST}")

# Ensure required variables are set
set(REQUIRED_VARS PROJECT_NAME PROJECT_VERSION PROJECT_DESCRIPTION)
foreach(VAR ${REQUIRED_VARS})
    if(NOT DEFINED ${VAR})
        message(FATAL_ERROR "${VAR} is not defined in config.cmake")
    endif()
endforeach()

# Ensure at least one of EXECUTABLE_TARGETS or LIBRARY_TARGETS is defined
if(NOT (DEFINED EXECUTABLE_TARGETS OR DEFINED LIBRARY_TARGETS))
    message(FATAL_ERROR "At least one of EXECUTABLE_TARGETS or LIBRARY_TARGETS must be defined.")
endif()

project(${PROJECT_NAME} VERSION ${PROJECT_VERSION} DESCRIPTION "${PROJECT_DESCRIPTION}" LANGUAGES ${PROJECT_LANGUAGE})

include(GNUInstallDirs)
include_directories(/usr/local/include)
link_directories(/usr/local/lib /usr/local/lib64)

# Print compiler information
message(STATUS "Using Compiler: ${CMAKE_C_COMPILER}")
get_filename_component(COMPILER_NAME "${CMAKE_C_COMPILER}" NAME)
message(STATUS "Compiler Name: ${COMPILER_NAME}")

# Define macro to ensure absolute paths
macro(MAKE_ABSOLUTE_PATHS VAR_NAME)
    set(TEMP_LIST "")
    foreach(FILE ${${VAR_NAME}})
        if(IS_ABSOLUTE ${FILE})
            list(APPEND TEMP_LIST ${FILE})
        else()
            list(APPEND TEMP_LIST "${CMAKE_SOURCE_DIR}/${FILE}")
        endif()
    endforeach()
    set(${VAR_NAME} ${TEMP_LIST})
    message(STATUS "Processed paths for ${VAR_NAME}: ${${VAR_NAME}}")
endmacro()

# Find required tools
find_program(CLANG_FORMAT NAMES ${CLANG_FORMAT_NAME})
find_program(CLANG_TIDY NAMES ${CLANG_TIDY_NAME})
find_program(CPPCHECK NAMES ${CPPCHECK_NAME})

message(STATUS "Found clang-format: ${CLANG_FORMAT}")
message(STATUS "Found clang-tidy: ${CLANG_TIDY}")
message(STATUS "Found cppcheck: ${CPPCHECK}")

if(NOT CLANG_FORMAT OR NOT CLANG_TIDY OR NOT CPPCHECK)
    message(FATAL_ERROR "Required tools (clang-format, clang-tidy, cppcheck) not found.")
endif()

# Function to load sanitizer flags
function(load_sanitizer_flags OUTPUT_VAR)
    if(NOT DEFINED SANITIZER_LIST OR "${SANITIZER_LIST}" STREQUAL "")
        set(${OUTPUT_VAR} "" PARENT_SCOPE)
        return()
    endif()

    set(SANITIZERS "")
    string(REPLACE "," ";" SANITIZER_NAMES "${SANITIZER_LIST}")
    foreach(SAN ${SANITIZER_NAMES})
        set(FLAG_FILE "${CMAKE_SOURCE_DIR}/.flags/${COMPILER_NAME}/${SAN}_sanitizer_flags.txt")
        if(EXISTS "${FLAG_FILE}")
            file(STRINGS "${FLAG_FILE}" RAW_LINES)
            foreach(RAW_LINE ${RAW_LINES})
                string(REPLACE " " ";" TOKENS "${RAW_LINE}")
                foreach(TOKEN ${TOKENS})
                    # Only process if it starts with -fsanitize=
                    if(TOKEN MATCHES "^-fsanitize=")
                        string(REPLACE "-fsanitize=" "" CLEANED "${TOKEN}")
                        string(REPLACE "," ";" INDIVIDUAL_SANITIZERS "${CLEANED}")
                        foreach(SINGLE_SANITIZER ${INDIVIDUAL_SANITIZERS})
                            list(APPEND SANITIZERS "${SINGLE_SANITIZER}")
                        endforeach()
                    endif()
                endforeach()
            endforeach()
        else()
            message(WARNING "⚠️ Sanitizer '${SAN}' requested but no flag file found: ${FLAG_FILE}")
        endif()
    endforeach()

    list(REMOVE_DUPLICATES SANITIZERS)
    list(JOIN SANITIZERS "," SANITIZER_STRING)
    set(${OUTPUT_VAR} "-fsanitize=${SANITIZER_STRING}" PARENT_SCOPE)
endfunction()

# Load sanitizer flags
load_sanitizer_flags(SANITIZER_FLAGS)

if(SANITIZER_FLAGS)
    message(STATUS "  SANITIZER_FLAGS: ${SANITIZER_FLAGS}")
endif()

set(ALL_SOURCE_FILES "")
set(ALL_HEADER_FILES "")

# Process Executables
if(DEFINED EXECUTABLE_TARGETS)
    foreach(EXE ${EXECUTABLE_TARGETS})
        MAKE_ABSOLUTE_PATHS(${EXE}_SOURCES)
        MAKE_ABSOLUTE_PATHS(${EXE}_HEADERS)

        list(APPEND ALL_SOURCE_FILES ${${EXE}_SOURCES})
        list(APPEND ALL_HEADER_FILES ${${EXE}_HEADERS})

        message(STATUS "Executable ${EXE}: Sources: ${${EXE}_SOURCES}, Headers: ${${EXE}_HEADERS}")

        add_executable(${EXE})
        target_sources(${EXE} PRIVATE ${${EXE}_SOURCES})
        target_include_directories(${EXE} PUBLIC
                $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        )
        target_compile_options(${EXE} PRIVATE ${STANDARD_FLAGS} ${SANITIZER_FLAGS})
        target_link_options(${EXE} PRIVATE ${SANITIZER_FLAGS})
        target_link_libraries(${EXE} PRIVATE ${${EXE}_LINK_LIBRARIES})
        add_dependencies(${EXE} format)
    endforeach()
endif()

# Process Libraries
if(DEFINED LIBRARY_TARGETS)
    foreach(LIB ${LIBRARY_TARGETS})
        MAKE_ABSOLUTE_PATHS(${LIB}_SOURCES)
        MAKE_ABSOLUTE_PATHS(${LIB}_HEADERS)

        list(APPEND ALL_SOURCE_FILES ${${LIB}_SOURCES})
        list(APPEND ALL_HEADER_FILES ${${LIB}_HEADERS})

        message(STATUS "Library ${LIB}: Sources: ${${LIB}_SOURCES}, Headers: ${${LIB}_HEADERS}")

        set(FINAL_LINK_LIBRARIES "")

        foreach(LINK_LIB ${${LIB}_LINK_LIBRARIES})
            if(TARGET ${LINK_LIB})
                message(STATUS "✅ Found project target: ${LINK_LIB}")
                list(APPEND FINAL_LINK_LIBRARIES ${LINK_LIB})
            else()
                set(_FIND_VAR "${LINK_LIB}_FOUND")
                find_library(${_FIND_VAR} NAMES ${LINK_LIB} PATHS /usr/local/lib /usr/local/lib64 /opt/homebrew/lib)

                if(NOT "${${_FIND_VAR}}" MATCHES "-NOTFOUND$")
                    message(STATUS "✅ Found system library: ${LINK_LIB} at ${${_FIND_VAR}}")
                    list(APPEND FINAL_LINK_LIBRARIES ${LINK_LIB})
                elseif("${LINK_LIB}" STREQUAL "iconv" AND APPLE AND (CMAKE_C_COMPILER_ID MATCHES ".*Clang"))
                    message(STATUS "🟡 Manually linking -liconv on macOS with Clang/AppleClang")
                    list(APPEND FINAL_LINK_LIBRARIES iconv)
                else()
                    message(WARNING "❌ Optional link library '${LINK_LIB}' for '${LIB}' not found. Skipping linking it.")
                endif()
            endif()
        endforeach()

        add_library(${LIB} SHARED)
        target_sources(${LIB} PRIVATE ${${LIB}_SOURCES})
        target_include_directories(${LIB} PUBLIC
                $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        )
        target_compile_options(${LIB} PRIVATE ${STANDARD_FLAGS} ${SANITIZER_FLAGS})
        target_link_options(${LIB} PRIVATE ${SANITIZER_FLAGS})
        target_link_libraries(${LIB} PRIVATE ${FINAL_LINK_LIBRARIES})
        add_dependencies(${LIB} format)
    endforeach()
endif()

# Formatting target
add_custom_target(format
        COMMAND ${CLANG_FORMAT} --style=file -i ${ALL_SOURCE_FILES} ${ALL_HEADER_FILES}
        COMMENT "Running clang-format"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMAND_EXPAND_LISTS
)

# Clang-Tidy Check
add_custom_target(clang-tidy-check
        COMMAND ${CLANG_TIDY} ${ALL_SOURCE_FILES} ${ALL_HEADER_FILES} -quiet --warnings-as-errors='*' -checks=*,-llvmlibc-restrict-system-libc-headers,-altera-struct-pack-align,-readability-identifier-length,-altera-unroll-loops,-cppcoreguidelines-init-variables,-cert-err33-c,-modernize-macro-to-enum,-bugprone-easily-swappable-parameters,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-altera-id-dependent-backward-branch,-concurrency-mt-unsafe,-misc-unused-parameters,-hicpp-signed-bitwise,-google-readability-todo,-cert-msc30-c,-cert-msc50-cpp,-readability-function-cognitive-complexity,-clang-analyzer-security.insecureAPI.strcpy,-cert-env33-c,-android-cloexec-accept,-clang-analyzer-security.insecureAPI.rand,-misc-include-cleaner,-llvm-header-guard,-cppcoreguidelines-macro-to-enum,-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-llvmlibc-implementation-in-namespace,-llvmlibc-callee-namespace -- ${CMAKE_C_FLAGS} ${STANDARD_FLAGS} -I${CMAKE_SOURCE_DIR}/include -I/usr/local/include
        COMMENT "Running clang-tidy on all source files"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMAND_EXPAND_LISTS
)

# Cppcheck Check
add_custom_target(cppcheck-check
        COMMAND ${CPPCHECK} --error-exitcode=1 --force --quiet --inline-suppr --library=posix --check-level=exhaustive --enable=all --suppress=missingIncludeSystem --suppress=unusedFunction --suppress=staticFunction --suppress=constParameterPointer --suppress=unmatchedSuppression --suppress=checkersReport -I${CMAKE_SOURCE_DIR}/include -I/usr/local/include ${ALL_SOURCE_FILES}
        COMMENT "Running cppcheck on all source files"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMAND_EXPAND_LISTS
)

# Run analyzer explicitly if using Clang
if(CMAKE_C_COMPILER MATCHES ".*clang.*")
    add_custom_target(clang-analyze
            COMMAND ${CMAKE_C_COMPILER} --analyze --analyzer-output text
            ${STANDARD_FLAGS}
            -Xclang -analyzer-checker=core
            -Xclang -analyzer-checker=security
            -Xclang -analyzer-disable-checker=security.insecureAPI.DeprecatedOrUnsafeBufferHandling
            -I${CMAKE_SOURCE_DIR}/include
            -- ${ALL_SOURCE_FILES}
            COMMENT "Running Clang Static Analyzer"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_dependencies(clang-analyze clang-tidy-check cppcheck-check)
    add_custom_target(check ALL DEPENDS clang-tidy-check cppcheck-check clang-analyze)
else()
    add_custom_target(check ALL DEPENDS clang-tidy-check cppcheck-check)
endif()

# Install targets
if(DEFINED LIBRARY_TARGETS)
    install(TARGETS ${LIBRARY_TARGETS}
            EXPORT ${PROJECT_NAME}Targets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(DEFINED EXECUTABLE_TARGETS)
    install(TARGETS ${EXECUTABLE_TARGETS}
            EXPORT ${PROJECT_NAME}Targets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
)

install(EXPORT ${PROJECT_NAME}Targets
        FILE ${PROJECT_NAME}Targets.cmake
        NAMESPACE ${PROJECT_NAME}::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)
