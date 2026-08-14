# Project metadata
set(PROJECT_NAME "p101_env")
set(PROJECT_VERSION "0.0.1")
set(PROJECT_DESCRIPTION "Runtime environment, tracing, fault injection, and event logging")
set(PROJECT_LANGUAGE "C")

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Common compiler flags
set(STANDARD_FLAGS
        -D_POSIX_C_SOURCE=200809L
        -D_XOPEN_SOURCE=700
        -Werror
)

set(DARWIN_STANDARD_FLAGS
        -D_DARWIN_C_SOURCE
)

set(LINUX_STANDARD_FLAGS
)

set(BSD_STANDARD_FLAGS
)

# Define library targets
set(LIBRARY_TARGETS p101_env)

# Source files for the library
set(p101_env_SOURCES
        src/env.c
)

# Header files for installation
set(p101_env_HEADERS
        include/p101_env/env.h
        include/p101_env/wrapper.h
)

# Linked libraries required for this project
set(p101_env_LINK_LIBRARIES
        p101_error
        p101_record
)
