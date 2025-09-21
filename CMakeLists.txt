cmake_minimum_required(VERSION 3.21)

# Load project configuration
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/config.cmake")
    message(FATAL_ERROR "Missing config.cmake file. This file is required to define project settings.")
endif()
include("${CMAKE_SOURCE_DIR}/config.cmake")

# Echo important inputs
message(STATUS "Loaded config.cmake variables:")
message(STATUS "  PROJECT_NAME: ${PROJECT_NAME}")
message(STATUS "  PROJECT_VERSION: ${PROJECT_VERSION}")
message(STATUS "  PROJECT_DESCRIPTION: ${PROJECT_DESCRIPTION}")
message(STATUS "  PROJECT_LANGUAGE: ${PROJECT_LANGUAGE}")
message(STATUS "  EXECUTABLE_TARGETS: ${EXECUTABLE_TARGETS}")
message(STATUS "  LIBRARY_TARGETS: ${LIBRARY_TARGETS}")
message(STATUS "  STANDARD_FLAGS: ${STANDARD_FLAGS}")
message(STATUS "  SANITIZER_LIST: ${SANITIZER_LIST}")

# Validate required variables including language
set(REQUIRED_VARS PROJECT_NAME PROJECT_VERSION PROJECT_DESCRIPTION PROJECT_LANGUAGE)
foreach(VAR ${REQUIRED_VARS})
    if(NOT DEFINED ${VAR} OR "${${VAR}}" STREQUAL "")
        message(FATAL_ERROR "${VAR} is not defined or empty in config.cmake")
    endif()
endforeach()

string(TOUPPER "${PROJECT_LANGUAGE}" _PLANG)
if(NOT _PLANG MATCHES "^(C|CXX)$")
    message(FATAL_ERROR "PROJECT_LANGUAGE must be C or CXX, got '${PROJECT_LANGUAGE}'")
endif()

# Require at least one nonempty target list
if((NOT EXECUTABLE_TARGETS) AND (NOT LIBRARY_TARGETS))
    message(FATAL_ERROR "At least one of EXECUTABLE_TARGETS or LIBRARY_TARGETS must be nonempty.")
endif()

project(${PROJECT_NAME} VERSION ${PROJECT_VERSION} DESCRIPTION "${PROJECT_DESCRIPTION}" LANGUAGES ${PROJECT_LANGUAGE})

# --- External include/lib prefixes we want visible everywhere ---
set(EXTERNAL_INCLUDE_DIRS "")
foreach(_inc IN ITEMS /usr/local/include)
    if(EXISTS "${_inc}")
        list(APPEND EXTERNAL_INCLUDE_DIRS "${_inc}")
    endif()
endforeach()

set(EXTERNAL_LIBRARY_DIRS "")
foreach(_lib IN ITEMS /usr/local/lib /usr/local/lib64)
    if(EXISTS "${_lib}")
        list(APPEND EXTERNAL_LIBRARY_DIRS "${_lib}")
    endif()
endforeach()

# Global include dirs (still helpful for ad-hoc invocations)
foreach(_inc ${EXTERNAL_INCLUDE_DIRS})
    include_directories(BEFORE SYSTEM "${_inc}")
    add_compile_options(-isystem "${_inc}")
endforeach()

# Global link search dirs
foreach(_lib ${EXTERNAL_LIBRARY_DIRS})
    link_directories("${_lib}")
endforeach()

# Interface target so every real target inherits the paths
add_library(sys_paths INTERFACE)
if(EXTERNAL_INCLUDE_DIRS)
    target_include_directories(sys_paths INTERFACE ${EXTERNAL_INCLUDE_DIRS})
endif()
if(EXTERNAL_LIBRARY_DIRS)
    target_link_directories(sys_paths INTERFACE ${EXTERNAL_LIBRARY_DIRS})
endif()

# Helpful for installed targets to inherit found link paths
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

# Ensure standard install dir variables exist before any install() or INSTALL_INTERFACE usage
include(GNUInstallDirs)
if(NOT DEFINED CMAKE_INSTALL_INCLUDEDIR OR "${CMAKE_INSTALL_INCLUDEDIR}" STREQUAL "")
    set(CMAKE_INSTALL_INCLUDEDIR include)
endif()
if(NOT DEFINED CMAKE_INSTALL_LIBDIR OR "${CMAKE_INSTALL_LIBDIR}" STREQUAL "")
    set(CMAKE_INSTALL_LIBDIR lib)
endif()
if(NOT DEFINED CMAKE_INSTALL_BINDIR OR "${CMAKE_INSTALL_BINDIR}" STREQUAL "")
    set(CMAKE_INSTALL_BINDIR bin)
endif()
message(STATUS "Install dirs: inc='${CMAKE_INSTALL_INCLUDEDIR}', lib='${CMAKE_INSTALL_LIBDIR}', bin='${CMAKE_INSTALL_BINDIR}'")

# Make all symbols visible by default
set(CMAKE_C_VISIBILITY_PRESET default)
set(CMAKE_CXX_VISIBILITY_PRESET default)
set(CMAKE_VISIBILITY_INLINES_HIDDEN OFF)

# Build options
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Defaults for tool names (script can override)
if(NOT DEFINED CLANG_FORMAT_NAME)
    set(CLANG_FORMAT_NAME clang-format)
endif()
if(NOT DEFINED CLANG_TIDY_NAME)
    set(CLANG_TIDY_NAME clang-tidy)
endif()
if(NOT DEFINED CPPCHECK_NAME)
    set(CPPCHECK_NAME cppcheck)
endif()

# Find tools
find_program(CLANG_FORMAT NAMES ${CLANG_FORMAT_NAME})
find_program(CLANG_TIDY   NAMES ${CLANG_TIDY_NAME})
find_program(CPPCHECK     NAMES ${CPPCHECK_NAME})

message(STATUS "Found clang-format: ${CLANG_FORMAT}")
message(STATUS "Found clang-tidy: ${CLANG_TIDY}")
message(STATUS "Found cppcheck: ${CPPCHECK}")

# Hard requirement
if(NOT CLANG_FORMAT OR NOT CLANG_TIDY OR NOT CPPCHECK)
    message(FATAL_ERROR "Required tools (clang-format, clang-tidy, cppcheck) not found.")
endif()

# Print compiler information
message(STATUS "Using Compiler: ${CMAKE_C_COMPILER}")
get_filename_component(COMPILER_NAME "${CMAKE_C_COMPILER}" NAME)
message(STATUS "Compiler Name: ${COMPILER_NAME}")

# Helper to absolutize file lists declared in config.cmake
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

# Load sanitizer flags by reading .flags/<compiler or ID>/<san>_sanitizer_flags.txt
function(load_sanitizer_flags OUTPUT_VAR)
    if(NOT DEFINED SANITIZER_LIST OR "${SANITIZER_LIST}" STREQUAL "")
        set(${OUTPUT_VAR} "" PARENT_SCOPE)
        return()
    endif()

    get_filename_component(_compiler_basename "${CMAKE_C_COMPILER}" NAME)
    set(_flags_dir "${CMAKE_SOURCE_DIR}/.flags/${_compiler_basename}")
    if(NOT EXISTS "${_flags_dir}")
        set(_flags_dir "${CMAKE_SOURCE_DIR}/.flags/${CMAKE_C_COMPILER_ID}")
    endif()

    if(APPLE AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
        message(WARNING "GCC on macOS cannot link sanitizer runtimes; ignoring SANITIZER_LIST='${SANITIZER_LIST}'. Use clang for sanitizer builds on macOS.")
        set(${OUTPUT_VAR} "" PARENT_SCOPE)
        return()
    endif()

    set(SANITIZERS "")
    string(REPLACE "," ";" SANITIZER_NAMES "${SANITIZER_LIST}")
    foreach(SAN ${SANITIZER_NAMES})
        set(FLAG_FILE "${_flags_dir}/${SAN}_sanitizer_flags.txt")
        if(EXISTS "${FLAG_FILE}")
            file(STRINGS "${FLAG_FILE}" RAW_LINES)
            foreach(RAW_LINE ${RAW_LINES})
                string(REPLACE " " ";" TOKENS "${RAW_LINE}")
                foreach(TOKEN ${TOKENS})
                    if(TOKEN MATCHES "^-fsanitize=")
                        string(REPLACE "-fsanitize=" "" CLEANED "${TOKEN}")
                        string(REPLACE "," ";" INDIVIDUAL_SANITIZERS "${CLEANED}")
                        list(APPEND SANITIZERS ${INDIVIDUAL_SANITIZERS})
                    endif()
                endforeach()
            endforeach()
        else()
            message(WARNING "Sanitizer '${SAN}' requested but no flag file found: ${FLAG_FILE}")
        endif()
    endforeach()

    list(REMOVE_DUPLICATES SANITIZERS)
    list(JOIN SANITIZERS "," SANITIZER_STRING)
    if(SANITIZER_STRING STREQUAL "")
        set(${OUTPUT_VAR} "" PARENT_SCOPE)
    else()
        set(${OUTPUT_VAR} "-fsanitize=${SANITIZER_STRING}" PARENT_SCOPE)
    endif()
endfunction()

# Resolve sanitizer flags and announce once
load_sanitizer_flags(SANITIZER_FLAGS)
if(SANITIZER_FLAGS)
    message(STATUS "  SANITIZER_FLAGS: ${SANITIZER_FLAGS}")
endif()

# Aggregate lists for tool targets
set(ALL_SOURCE_FILES "")
set(ALL_HEADER_FILES "")

# Interface target for common options
add_library(project_options INTERFACE)

# Keep legacy linker search convenience on all platforms
foreach(_lib ${EXTERNAL_LIBRARY_DIRS})
    target_link_directories(project_options INTERFACE "${_lib}")
endforeach()

target_compile_options(project_options INTERFACE ${STANDARD_FLAGS})
if(SANITIZER_FLAGS)
    target_compile_options(project_options INTERFACE ${SANITIZER_FLAGS} -fno-omit-frame-pointer)
    target_link_options(project_options    INTERFACE ${SANITIZER_FLAGS})
endif()

# Executables
if(EXECUTABLE_TARGETS)
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

        # Resolve and log system libraries, still link by the given names
        set(_EXE_FINAL_LIBS "")
        foreach(LINK_LIB ${${EXE}_LINK_LIBRARIES})
            if(TARGET ${LINK_LIB})
                list(APPEND _EXE_FINAL_LIBS ${LINK_LIB})
            else()
                set(_FIND_VAR "${EXE}_${LINK_LIB}_FOUND")
                find_library(${_FIND_VAR} NAMES ${LINK_LIB} PATHS /usr/local/lib /usr/local/lib64)
                if(NOT "${${_FIND_VAR}}" MATCHES "-NOTFOUND$")
                    message(STATUS "✅ Found system library for executable ${EXE}: ${LINK_LIB} at ${${_FIND_VAR}}")
                elseif("${LINK_LIB}" STREQUAL "iconv" AND APPLE AND (CMAKE_C_COMPILER_ID MATCHES ".*Clang"))
                    message(STATUS "🟡 Manually linking -liconv for executable ${EXE} on macOS with Clang/AppleClang")
                else()
                    message(WARNING "Link library '${LINK_LIB}' for executable '${EXE}' not found by find_library. Relying on default linker search paths.")
                endif()
                list(APPEND _EXE_FINAL_LIBS ${LINK_LIB})
            endif()
        endforeach()

        target_link_libraries(${EXE} PRIVATE project_options sys_paths ${_EXE_FINAL_LIBS})
        if(TARGET format)
            add_dependencies(${EXE} format)
        endif()
    endforeach()
endif()

# Libraries
if(LIBRARY_TARGETS)
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
                set(_FIND_VAR "${LIB}_${LINK_LIB}_FOUND")
                find_library(${_FIND_VAR} NAMES ${LINK_LIB} PATHS /usr/local/lib /usr/local/lib64)
                if(NOT "${${_FIND_VAR}}" MATCHES "-NOTFOUND$")
                    message(STATUS "✅ Found system library: ${LINK_LIB} at ${${_FIND_VAR}}")
                    list(APPEND FINAL_LINK_LIBRARIES ${LINK_LIB}) # link by name to keep RPATH behavior
                elseif("${LINK_LIB}" STREQUAL "iconv" AND APPLE AND (CMAKE_C_COMPILER_ID MATCHES ".*Clang"))
                    message(STATUS "🟡 Manually linking -liconv on macOS with Clang/AppleClang")
                    list(APPEND FINAL_LINK_LIBRARIES iconv)
                else()
                    message(WARNING "❌ Optional link library '${LINK_LIB}' for '${LIB}' not found. Skipping linking it.")
                endif()
            endif()
        endforeach()

        add_library(${LIB})
        target_sources(${LIB} PRIVATE ${${LIB}_SOURCES})
        target_include_directories(${LIB} PUBLIC
                $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        )
        target_link_libraries(${LIB} PRIVATE project_options sys_paths ${FINAL_LINK_LIBRARIES})
        if(TARGET format)
            add_dependencies(${LIB} format)
        endif()
    endforeach()
endif()

# Formatting target
if(ALL_SOURCE_FILES OR ALL_HEADER_FILES)
    add_custom_target(format
            COMMAND ${CLANG_FORMAT} --style=file -i ${ALL_SOURCE_FILES} ${ALL_HEADER_FILES}
            COMMENT "Running clang-format"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMAND_EXPAND_LISTS
    )
endif()

# Build a reusable list of -I flags for tool commands (analyzer / clang-tidy)
set(TOOL_EXTRA_I_FLAGS -I${CMAKE_SOURCE_DIR}/include)
foreach(_inc ${EXTERNAL_INCLUDE_DIRS})
    list(APPEND TOOL_EXTRA_I_FLAGS -I${_inc})
endforeach()

# Clang-Tidy target (now also sees external include dirs)
if(ALL_SOURCE_FILES OR ALL_HEADER_FILES)
    add_custom_target(clang-tidy-check
            COMMAND ${CLANG_TIDY} ${ALL_SOURCE_FILES} ${ALL_HEADER_FILES}
            -quiet --warnings-as-errors='*'
            -checks=*,-llvmlibc-restrict-system-libc-headers,-altera-struct-pack-align,-readability-identifier-length,-altera-unroll-loops,-cppcoreguidelines-init-variables,-cert-err33-c,-modernize-macro-to-enum,-bugprone-easily-swappable-parameters,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-altera-id-dependent-backward-branch,-concurrency-mt-unsafe,-misc-unused-parameters,-hicpp-signed-bitwise,-google-readability-todo,-cert-msc30-c,-cert-msc50-cpp,-readability-function-cognitive-complexity,-clang-analyzer-security.insecureAPI.strcpy,-cert-env33-c,-android-cloexec-accept,-clang-analyzer-security.insecureAPI.rand,-misc-include-cleaner,-llvm-header-guard,-cppcoreguidelines-macro-to-enum,-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-llvmlibc-implementation-in-namespace,-llvmlibc-callee-namespace
            -- ${CMAKE_C_FLAGS} ${STANDARD_FLAGS} ${TOOL_EXTRA_I_FLAGS}
            COMMENT "Running clang-tidy on all source files"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMAND_EXPAND_LISTS
    )
endif()

# Cppcheck target
if(ALL_SOURCE_FILES OR ALL_HEADER_FILES)
    add_custom_target(cppcheck-check
            COMMAND ${CPPCHECK} --error-exitcode=1 --force --quiet --inline-suppr --library=posix
            --check-level=exhaustive --enable=all
            --suppress=missingIncludeSystem --suppress=unusedFunction --suppress=staticFunction
            --suppress=constParameterPointer --suppress=unmatchedSuppression --suppress=checkersReport
            ${TOOL_EXTRA_I_FLAGS} ${ALL_SOURCE_FILES}
            COMMENT "Running cppcheck on all source files"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMAND_EXPAND_LISTS
    )
endif()

# Clang Static Analyzer (now includes external includes so p101_error resolves)
if(CMAKE_C_COMPILER MATCHES ".*clang.*" AND (ALL_SOURCE_FILES OR ALL_HEADER_FILES))
    add_custom_target(clang-analyze
            COMMAND ${CMAKE_C_COMPILER} --analyze --analyzer-output text
            ${CMAKE_C_FLAGS} ${STANDARD_FLAGS}
            -Xclang -analyzer-checker=core
            -Xclang -analyzer-checker=security
            -Xclang -analyzer-disable-checker=security.insecureAPI.DeprecatedOrUnsafeBufferHandling
            ${TOOL_EXTRA_I_FLAGS} -- ${ALL_SOURCE_FILES}
            COMMENT "Running Clang Static Analyzer"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMAND_EXPAND_LISTS
    )
    if(TARGET clang-tidy-check)
        add_dependencies(clang-analyze clang-tidy-check)
    endif()
    if(TARGET cppcheck-check)
        add_dependencies(clang-analyze cppcheck-check)
    endif()
endif()

# Aggregate check target
add_custom_target(check ALL)
if(TARGET clang-tidy-check)
    add_dependencies(check clang-tidy-check)
endif()
if(TARGET cppcheck-check)
    add_dependencies(check cppcheck-check)
endif()
if(TARGET clang-analyze)
    add_dependencies(check clang-analyze)
endif()

# Install rules
if(LIBRARY_TARGETS)
    install(TARGETS ${LIBRARY_TARGETS}
            EXPORT ${PROJECT_NAME}Targets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(EXECUTABLE_TARGETS)
    install(TARGETS ${EXECUTABLE_TARGETS}
            EXPORT ${PROJECT_NAME}Targets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
)

if(EXECUTABLE_TARGETS OR LIBRARY_TARGETS)
    install(EXPORT ${PROJECT_NAME}Targets
            FILE ${PROJECT_NAME}Targets.cmake
            NAMESPACE ${PROJECT_NAME}::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
    )
endif()
