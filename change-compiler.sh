#!/usr/bin/env bash
set -euo pipefail

c_compiler=""
clang_format_name="clang-format"
clang_tidy_name="clang-tidy"
cppcheck_name="cppcheck"
sanitizers=""
sanitizers_passed=false

usage() {
  echo "Usage: $0 -c <c compiler> [-f <clang-format>] [-t <clang-tidy>] [-k <cppcheck>] [-s <sanitizers>]" >&2
  echo "  -c c compiler    Specify the C compiler name, for example gcc or clang" >&2
  echo "  -f clang-format  Specify the clang-format name, for example clang-format-17" >&2
  echo "  -t clang-tidy    Specify the clang-tidy name, for example clang-tidy-17" >&2
  echo "  -k cppcheck      Specify the cppcheck name, for example cppcheck" >&2
  echo "  -s sanitizers    Comma list, for example address,undefined. If omitted, uses sanitizers.txt" >&2
  exit 1
}

while getopts ":c:f:t:k:s:" opt; do
  case $opt in
    c) c_compiler="$OPTARG" ;;
    f) clang_format_name="$OPTARG" ;;
    t) clang_tidy_name="$OPTARG" ;;
    k) cppcheck_name="$OPTARG" ;;
    s) sanitizers="$OPTARG"; sanitizers_passed=true ;;
    \?|:) usage ;;
  esac
done

if [ -z "$c_compiler" ]; then
  echo "Error: C compiler argument (-c) is required." >&2
  usage
fi

if ! $sanitizers_passed; then
  if [ -f "sanitizers.txt" ]; then
    # Strip all whitespace and ignore comments after '#'
    sanitizers="$(sed 's/#.*$//g' sanitizers.txt | tr -d '[:space:]')"
    echo "Sanitizers loaded from sanitizers.txt: ${sanitizers:-<none>}"
  else
    echo "Warning: sanitizers.txt not found and no -s provided. Using no sanitizers."
    sanitizers=""
  fi
else
  echo "Sanitizers specified via command-line: ${sanitizers:-<none>}"
fi

# Clean build dir to avoid stale cache
rm -rf build
mkdir -p build

echo "Configuring with:"
echo "  CC               = $c_compiler"
echo "  clang-format     = $clang_format_name"
echo "  clang-tidy       = $clang_tidy_name"
echo "  cppcheck         = $cppcheck_name"
echo "  sanitizers       = ${sanitizers:-<none>}"

cmake -S . -B build \
  -DCMAKE_C_COMPILER="$c_compiler" \
  -DCLANG_FORMAT_NAME="$clang_format_name" \
  -DCLANG_TIDY_NAME="$clang_tidy_name" \
  -DCPPCHECK_NAME="$cppcheck_name" \
  -DSANITIZER_LIST="$sanitizers" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
