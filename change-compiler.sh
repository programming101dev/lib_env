#!/usr/bin/env bash

c_compiler=""
clang_format_name="clang-format"
clang_tidy_name="clang-tidy"
cppcheck_name="cppcheck"
sanitizers=""
sanitizers_passed=false

# Function to display script usage
usage()
{
    echo "Usage: $0 -c <c compiler> [-f <clang-format>] [-t <clang-tidy>] [-k <cppcheck>] [-s <sanitizers>]"
    echo "  -c c compiler    Specify the C compiler name (e.g. gcc or clang)"
    echo "  -f clang-format  Specify the clang-format name (e.g. clang-format-17)"
    echo "  -t clang-tidy    Specify the clang-tidy name (e.g. clang-tidy-17)"
    echo "  -k cppcheck      Specify the cppcheck name (e.g. cppcheck)"
    echo "  -s sanitizers    Specify sanitizers manually (e.g. address,undefined). If omitted, uses sanitizers.txt"
    exit 1
}

# Parse command-line options
while getopts ":c:f:t:k:s:" opt; do
  case $opt in
    c)
      c_compiler="$OPTARG"
      ;;
    f)
      clang_format_name="$OPTARG"
      ;;
    t)
      clang_tidy_name="$OPTARG"
      ;;
    k)
      cppcheck_name="$OPTARG"
      ;;
    s)
      sanitizers="$OPTARG"
      sanitizers_passed=true
      ;;
    \?)
      echo "Invalid option: -$OPTARG" >&2
      usage
      ;;
    :)
      echo "Option -$OPTARG requires an argument." >&2
      usage
      ;;
  esac
done

# Ensure a compiler is provided
if [ -z "$c_compiler" ]; then
  echo "Error: C compiler argument (-c) is required."
  usage
fi

# Check if sanitizers.txt should be used
if ! $sanitizers_passed; then
    if [ -f "sanitizers.txt" ]; then
        sanitizers=$(tr -d ' \n' < sanitizers.txt)  # Remove spaces and newlines
        echo "Sanitizers loaded from sanitizers.txt: $sanitizers"
    else
        echo "Warning: sanitizers.txt not found and no sanitizers provided via -s option. Defaulting to none."
        sanitizers=""
    fi
else
    echo "Sanitizers specified via command-line: $sanitizers"
fi

# Pass sanitizers as a single variable
rm -rf build/CMakeCache.txt
cmake -S . -B build \
    -DCMAKE_C_COMPILER="$c_compiler" \
    -DCLANG_FORMAT_NAME="$clang_format_name" \
    -DCLANG_TIDY_NAME="$clang_tidy_name" \
    -DCPPCHECK_NAME="$cppcheck_name" \
    -DSANITIZER_LIST="$sanitizers" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_OSX_SYSROOT=""
