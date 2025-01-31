#!/usr/bin/env bash

NAME="p101_env"

# Check if the directory exists before attempting to delete
if [ -d "/usr/local/include/${NAME}" ]; then
    rm -rf "/usr/local/include/${NAME}"
fi

# Check if any files exist matching the pattern before attempting to delete
if ls /usr/local/lib/lib${NAME}* 1> /dev/null 2>&1; then
    rm /usr/local/lib/lib${NAME}*
fi

# Check if the lib64 directory exists and if any files match the pattern before attempting to delete
if [ -d "/usr/local/lib64" ] && ls /usr/local/lib64/lib${NAME}* 1> /dev/null 2>&1; then
    rm /usr/local/lib64/lib${NAME}*
fi
