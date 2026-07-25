#!/usr/bin/env bash

# Exit the script if any command fails
set -e

# --- opt-in coverage / profiling (P101) ---------------------------------
# Pull the long flags out before the normal option parser and export them.
# The shared CMakeLists reads P101_COVERAGE / P101_PROFILE at configure time
# and instruments the compile + link. Absent => nothing changes. If a parent
# (e.g. update.sh / build-all.sh) already exported them, they are inherited.
_p101_argv=()
for _p101_a in "$@"; do
  case "$_p101_a" in
    --coverage) export P101_COVERAGE=1 ;;
    --profile)  export P101_PROFILE=1 ;;
    *)          _p101_argv+=("$_p101_a") ;;
  esac
done
if ((${#_p101_argv[@]})); then set -- "${_p101_argv[@]}"; else set --; fi
unset _p101_argv _p101_a
# ------------------------------------------------------------------------

c_compiler=""
clang_format_name="clang-format"
clang_tidy_name="clang-tidy"
cppcheck_name="cppcheck"

# Function to display script usage
usage()
{
    echo "Usage: $0 [-f <clang-format>] [-t <clang-tidy>] [-k <cppcheck>] [--coverage] [--profile]"
    echo "  -f clang-format   Specify the clang-format name (e.g. clang-tidy or clang-tidy-17)"
    echo "  -t clang-tidy     Specify the clang-tidy name (e.g. clang-tidy or clang-tidy-17)"
    echo "  -k cppcheck       Specify the cppcheck name (e.g. cppcheck)"
    echo "  --coverage        Instrument every build for code coverage (gcov)"
    echo "  --profile         Instrument every build for profiling (gprof)"
    exit 1
}

# --help / -h -> usage, exit 0 (P101 uniform CLI help)
case " $* " in *" --help "*|*" -h "*) ( usage ) || true; exit 0 ;; esac

# Parse command-line options using getopt
while getopts ":f:t:k:" opt; do
  case $opt in
    f)
      clang_format_name="$OPTARG"
      ;;
    t)
      clang_tidy_name="$OPTARG"
      ;;
    k)
      cppcheck_name="$OPTARG"
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

# Read the C compilers from the file into an array
c_compilers=()
while IFS= read -r line; do
    c_compilers+=("$line")
done < "supported_c_compilers.txt"

# Loop through the C compilers array
for c_compiler in "${c_compilers[@]}"; do
    echo "$c_compiler"

    ./change-compiler.sh -c "$c_compiler" -f "$clang_format_name" -t "$clang_tidy_name" -k "$cppcheck_name"
    ./build.sh
done
