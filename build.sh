#!/usr/bin/env bash
set -euo pipefail

# Defaults
jobs="${JOBS:-${CMAKE_BUILD_PARALLEL_LEVEL:-}}"
target=""
build_dir="build"

usage() {
  echo "Usage: $0 [-j N] [-t <target>]"
  echo "  -j N        Parallel build with N jobs (or set JOBS / CMAKE_BUILD_PARALLEL_LEVEL)"
  echo "  -t target   Build a specific target (e.g. -t main)"
  exit 1
}

# Parse options
while getopts ":j:t:h" opt; do
  case "$opt" in
    j) jobs="$OPTARG" ;;
    t) target="$OPTARG" ;;
    h|*) usage ;;
  esac
done

# Sanity checks
if [[ ! -d "$build_dir" || ! -f "$build_dir/CMakeCache.txt" ]]; then
  echo "You must run ./change-compiler.sh first (no configured build directory found)." >&2
  exit 1
fi

# Assemble build command
cmd=(cmake --build "$build_dir" --clean-first --verbose)
[[ -n "$target" ]] && cmd+=(--target "$target")
[[ -n "$jobs" ]] && cmd+=(--parallel "$jobs")

echo "Running: ${cmd[*]}"
exec "${cmd[@]}"
