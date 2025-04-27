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

# Ensure required variables are set
set(REQUIRED_VARS PROJECT_NAME PROJECT_VERSION PROJECT_DESCRIPTION)
foreach(VAR ${REQUIRED_VARS})
    if(NOT DEFINED ${VAR})
        message(FATAL_ERROR "${VAR} is not defined in config.cmake")
    endif()
endforeach()

# Print loaded project information
message(STATUS "Project Name: ${PROJECT_NAME}")
message(STATUS "Project Version: ${PROJECT_VERSION}")
message(STATUS "Project Description: ${PROJECT_DESCRIPTION}")

# Ensure at least one of EXECUTABLE_TARGETS or LIBRARY_TARGETS is defined
if(NOT (DEFINED EXECUTABLE_TARGETS OR DEFINED LIBRARY_TARGETS))
    message(FATAL_ERROR "At least one of EXECUTABLE_TARGETS or LIBRARY_TARGETS must be defined.")
endif()

project(${PROJECT_NAME} VERSION ${PROJECT_VERSION} DESCRIPTION ${PROJECT_DESCRIPTION} LANGUAGES ${PROJECT_LANGUAGE})

include(GNUInstallDirs)
include_directories(/usr/local/include)
link_directories(/usr/local/lib /usr/local/lib64)

# Print compiler information
message(STATUS "Using Compiler: ${CMAKE_C_COMPILER}")

# Extract the compiler name from the full path
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

# Find required tools dynamically
find_program(CLANG_FORMAT NAMES ${CLANG_FORMAT_NAME})
find_program(CLANG_TIDY NAMES ${CLANG_TIDY_NAME})
find_program(CPPCHECK NAMES ${CPPCHECK_NAME})

# Print tool paths
message(STATUS "Found clang-format: ${CLANG_FORMAT}")
message(STATUS "Found clang-tidy: ${CLANG_TIDY}")
message(STATUS "Found cppcheck: ${CPPCHECK}")

if(NOT CLANG_FORMAT OR NOT CLANG_TIDY OR NOT CPPCHECK)
    message(FATAL_ERROR "Required tools (clang-format, clang-tidy, cppcheck) not found.")
endif()

# Process sanitizers from `SANITIZER_LIST`
set(SANITIZER_FLAGS "")
if(DEFINED SANITIZER_LIST AND NOT "${SANITIZER_LIST}" STREQUAL "")
    string(REPLACE "," ";" SANITIZER_NAMES "${SANITIZER_LIST}")
    message(STATUS "Sanitizers enabled: ${SANITIZER_NAMES}")

    foreach(SAN ${SANITIZER_NAMES})
        set(FLAG_FILE "${CMAKE_SOURCE_DIR}/.flags/${COMPILER_NAME}/${SAN}_sanitizer_flags.txt")
        if(EXISTS "${FLAG_FILE}")
            file(STRINGS "${FLAG_FILE}" SANITIZER_FLAGS_CONTENTS)
            list(APPEND SANITIZER_FLAGS ${SANITIZER_FLAGS_CONTENTS})
        else()
            message(WARNING "⚠️ Sanitizer '${SAN}' requested but no flag file found: ${FLAG_FILE}")
        endif()
    endforeach()
endif()

# Ensure `ALL_SOURCE_FILES` is set before formatting
set(ALL_SOURCE_FILES "")
set(ALL_HEADER_FILES "")

# Process Executables
if(DEFINED EXECUTABLE_TARGETS)
    foreach(EXE ${EXECUTABLE_TARGETS})
        MAKE_ABSOLUTE_PATHS(${EXE}_SOURCES)
        MAKE_ABSOLUTE_PATHS(${EXE}_HEADERS)

        list(APPEND ALL_SOURCE_FILES ${${EXE}_SOURCES})
        list(APPEND ALL_HEADER_FILES ${${EXE}_HEADERS})

        message(STATUS "Executable ${EXE}:")
        message(STATUS "  Sources: ${${EXE}_SOURCES}")
        message(STATUS "  Headers: ${${EXE}_HEADERS}")
        message(STATUS "  Link Libraries: ${${EXE}_LINK_LIBRARIES}")
    endforeach()
endif()

# Process Libraries
if(DEFINED LIBRARY_TARGETS)
    foreach(LIB ${LIBRARY_TARGETS})
        MAKE_ABSOLUTE_PATHS(${LIB}_SOURCES)
        MAKE_ABSOLUTE_PATHS(${LIB}_HEADERS)

        list(APPEND ALL_SOURCE_FILES ${${LIB}_SOURCES})
        list(APPEND ALL_HEADER_FILES ${${LIB}_HEADERS})

        message(STATUS "Library ${LIB}:")
        message(STATUS "  Sources: ${${LIB}_SOURCES}")
        message(STATUS "  Headers: ${${LIB}_HEADERS}")
        message(STATUS "  Link Libraries: ${${LIB}_LINK_LIBRARIES}")

        # Create a list that will actually be linked
        set(FINAL_LINK_LIBRARIES "")

        foreach(LINK_LIB ${${LIB}_LINK_LIBRARIES})
            if(TARGET ${LINK_LIB})
                list(APPEND FINAL_LINK_LIBRARIES ${LINK_LIB})
                message(STATUS "✅ Found project target: ${LINK_LIB}")
            else()
                # If it's a well-known system library like "iconv", assume it exists
                if(${LINK_LIB} STREQUAL "iconv")
                    list(APPEND FINAL_LINK_LIBRARIES ${LINK_LIB})
                    message(STATUS "✅ Trusting system library: ${LINK_LIB}")
                else()
                    find_library(${LINK_LIB}_FOUND ${LINK_LIB} PATHS /usr/local/lib /usr/local/lib64)
                    if(${LINK_LIB}_FOUND)
                        list(APPEND FINAL_LINK_LIBRARIES ${LINK_LIB})
                        message(STATUS "✅ Found system library: ${LINK_LIB} at ${${LINK_LIB}_FOUND}")
                    else()
                        message(WARNING "⚠️ Link library '${LINK_LIB}' for '${LIB}' not found. Assuming it is built into libc or not needed on this platform.")
                    endif()
                endif()
            endif()
        endforeach()

        message(STATUS "📦 Final link libraries for ${LIB}: ${FINAL_LINK_LIBRARIES}")

        # Now actually add the library target
        add_library(${LIB} SHARED ${${LIB}_SOURCES})
        target_include_directories(${LIB} PUBLIC "${CMAKE_SOURCE_DIR}/include")
        target_link_libraries(${LIB} PRIVATE ${FINAL_LINK_LIBRARIES})
        target_compile_options(${LIB} PRIVATE ${STANDARD_FLAGS})
        add_dependencies(${LIB} format)
    endforeach()
endif()

# Define Formatting Target (Runs First)
add_custom_target(format
        COMMAND ${CLANG_FORMAT} --style=file -i ${ALL_SOURCE_FILES} ${ALL_HEADER_FILES}
        COMMENT "Running clang-format"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# Process Executables (Compile Happens Here)
if(DEFINED EXECUTABLE_TARGETS)
    foreach(EXE ${EXECUTABLE_TARGETS})
        add_executable(${EXE} ${${EXE}_SOURCES})
        target_include_directories(${EXE} PUBLIC "${CMAKE_SOURCE_DIR}/include")
        target_link_libraries(${EXE} PRIVATE ${${EXE}_LINK_LIBRARIES})
        target_compile_options(${EXE} PRIVATE ${STANDARD_FLAGS})
        add_dependencies(${EXE} format)
    endforeach()
endif()

# Define Clang-Tidy Check
add_custom_target(clang-tidy-check
        COMMAND ${CLANG_TIDY} ${ALL_SOURCE_FILES} ${ALL_HEADER_FILES} -quiet --warnings-as-errors='*' -checks=*,-llvmlibc-restrict-system-libc-headers,-altera-struct-pack-align,-readability-identifier-length,-altera-unroll-loops,-cppcoreguidelines-init-variables,-cert-err33-c,-modernize-macro-to-enum,-bugprone-easily-swappable-parameters,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-altera-id-dependent-backward-branch,-concurrency-mt-unsafe,-misc-unused-parameters,-hicpp-signed-bitwise,-google-readability-todo,-cert-msc30-c,-cert-msc50-cpp,-readability-function-cognitive-complexity,-clang-analyzer-security.insecureAPI.strcpy,-cert-env33-c,-android-cloexec-accept,-clang-analyzer-security.insecureAPI.rand,-misc-include-cleaner,-llvm-header-guard,-cppcoreguidelines-macro-to-enum,-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling -- ${CMAKE_C_FLAGS} ${STANDARD_FLAGS} -I${CMAKE_SOURCE_DIR}/include -I/usr/local/include
        COMMENT "Running clang-tidy on all source files"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# Define Cppcheck Check
add_custom_target(cppcheck-check
        COMMAND ${CPPCHECK} --error-exitcode=1 --force --quiet --inline-suppr --library=posix --check-level=exhaustive --enable=all --suppress=missingIncludeSystem --suppress=unusedFunction --suppress=staticFunction --suppress=constParameterPointer --suppress=unmatchedSuppression --suppress=checkersReport -I${CMAKE_SOURCE_DIR}/include -I/usr/local/include ${ALL_SOURCE_FILES}
        COMMENT "Running cppcheck on all source files"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# Run Clang Analyzer explicitly after linting (only if using Clang)
if (CMAKE_C_COMPILER MATCHES ".*clang.*")
    add_custom_target(clang-analyze
            COMMAND ${CMAKE_C_COMPILER} --analyze --analyzer-output text
            -Xclang -analyzer-checker=core
            -Xclang -analyzer-checker=security
            -Xclang -analyzer-disable-checker=security.insecureAPI.DeprecatedOrUnsafeBufferHandling
            ${CMAKE_C_FLAGS}
            ${STANDARD_FLAGS}
            -I${CMAKE_SOURCE_DIR}/include
            -- ${ALL_SOURCE_FILES}
            COMMENT "Running Clang Static Analyzer on all files"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_dependencies(clang-analyze clang-tidy-check cppcheck-check)
    add_custom_target(check ALL DEPENDS clang-tidy-check cppcheck-check clang-analyze)
else()
    add_custom_target(check ALL DEPENDS clang-tidy-check cppcheck-check)
endif()

if(DEFINED LIBRARY_TARGETS)
    install(TARGETS ${LIBRARY_TARGETS}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(DEFINED EXECUTABLE_TARGETS)
    install(TARGETS ${EXECUTABLE_TARGETS}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
)
