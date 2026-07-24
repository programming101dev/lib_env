#!/usr/bin/env bash
# build.sh — build the last configured CMake build directory
# Supports: macOS, modern Linux, modern FreeBSD
set -euo pipefail

# QoL: -q / --quiet hides the per-file compile-command dump.
_P101_VERBOSE=1
_p101_bq=()
for _p101_bqa in "$@"; do
  case "$_p101_bqa" in
    -q|--quiet) export P101_QUIET=1; _P101_VERBOSE= ;;
    *) _p101_bq+=("$_p101_bqa") ;;
  esac
done
if ((${#_p101_bq[@]})); then set -- "${_p101_bq[@]}"; else set --; fi
unset _p101_bq _p101_bqa


# ----------------- defaults -----------------
jobs="${JOBS:-${CMAKE_BUILD_PARALLEL_LEVEL:-}}"
target=""
build_dir=""

usage() {
  echo "Usage: $0 [-j N] [-t <target>] [-q]"
  echo "  -j N        Parallel build with N jobs (or set JOBS / CMAKE_BUILD_PARALLEL_LEVEL)"
  echo "  -t target   Build a specific target (e.g. -t main)"
  echo "  -f, --format Apply clang-tidy --fix (full check set) + clang-format, then exit"
  echo "  -C, --format-check  Check formatting only (clang-format --dry-run); non-zero if unclean"
  echo "  -q          Quiet: hide the per-file compile-command dump"
  exit 1
}

# --help / -h -> usage, exit 0 (P101 uniform CLI help)
case " $* " in *" --help "*|*" -h "*) ( usage ) || true; exit 0 ;; esac

# -f / --format : apply clang-tidy --fix (full check set) + clang-format, then
# exit. Resolves the configured build dir on its own (independent of the options
# below) so it works regardless of how this build.sh reads its build dir.
case " $* " in
  *" -f "*|*" --format "*)
    _fbd="build"; [[ -f .last-build-dir ]] && _fbd="$(< .last-build-dir)"
    if [[ ! -d "$_fbd" || ! -f "$_fbd/CMakeCache.txt" ]]; then
      echo "No configured build dir ('$_fbd'). Run ./change-compiler.sh first." >&2
      exit 1
    fi
    echo "Formatting (clang-tidy --fix + clang-format) via target 'format' in '$_fbd'"
    exec cmake --build "$_fbd" --target format
    ;;
esac

# -C / --format-check : verify formatting WITHOUT modifying anything (fast, no
# build needed) — ideal for a git pre-commit hook. Non-zero exit if any file
# would be reformatted. clang-format is optional; absent => skip (exit 0).
case " $* " in
  *" -C "*|*" --format-check "*)
    if ! command -v clang-format >/dev/null 2>&1; then
      echo "clang-format not found; skipping format check." >&2; exit 0
    fi
    _fcfiles=$(find src include -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' -o -name '*.cc' -o -name '*.hh' \) 2>/dev/null)
    if [[ -z "$_fcfiles" ]]; then echo "format-check: no sources found."; exit 0; fi
    if clang-format --dry-run --Werror -style=file $_fcfiles; then
      echo "format-check: clean."
      exit 0
    else
      echo "format-check: files above need formatting — run ./build.sh --format" >&2
      exit 1
    fi
    ;;
esac

# ----------------- parse options -----------------
while getopts ":j:t:h" opt; do
  case "$opt" in
    j) jobs="$OPTARG" ;;
    t) target="$OPTARG" ;;
    h|*) usage ;;
  esac
done

# ----------------- determine build dir -----------------
# Preferred: read the last configured build dir written by change-compiler.sh
if [[ -f ".last-build-dir" ]]; then
  build_dir="$(< .last-build-dir)"
else
  # Fallback for legacy/manual setups
  build_dir="build"
fi

# ----------------- sanity checks -----------------
if [[ ! -d "$build_dir" || ! -f "$build_dir/CMakeCache.txt" ]]; then
  echo "Error: build directory '$build_dir' is not configured." >&2
  echo "Run ./change-compiler.sh first." >&2
  exit 1
fi

# ----------------- assemble build command -----------------
cmd=(cmake --build "$build_dir" --clean-first ${_P101_VERBOSE:+--verbose})
[[ -n "$target" ]] && cmd+=(--target "$target")
[[ -n "$jobs" ]] && cmd+=(--parallel "$jobs")

# ----------------- run -----------------
echo "Using build directory: $build_dir"
echo "Running: ${cmd[*]}"
exec "${cmd[@]}"
