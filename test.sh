#!/usr/bin/env bash
# test.sh — build and run this project's Unity test suite via ctest.
# Tests live in a SEPARATE CMake tree under test/, so vendored Unity and the
# test code are not subjected to the project's strict analysis build. Uses the
# same compiler the main build is using (from .last-build-dir). C and C++.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

usage() {
  cat <<'USAGE'
Usage: ./test.sh [--coverage] [-- <ctest args>]
  Configures/builds test/ with the compiler from the last main build
  (./change-compiler.sh), then runs the Unity tests through ctest.
  --coverage      instrument the code under test for ./coverage-report.sh.
  -- <args>       pass the rest through to ctest (e.g. -- -R display -V).
USAGE
}
coverage=0; ctest_args=()
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --coverage) coverage=1; shift ;;
    --) shift; ctest_args=("$@"); break ;;
    *) echo "Unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

[ -d test ] && [ -f test/CMakeLists.txt ] || { echo "No test/ tree here." >&2; exit 1; }

main_bd="build"; [ -f .last-build-dir ] && main_bd="$(cat .last-build-dir)"
[ -f "$main_bd/CMakeCache.txt" ] || { echo "No configured main build ('$main_bd'). Run ./change-compiler.sh first." >&2; exit 1; }

# project language decides which compiler the test tree needs
lang="C"; [ -f config.cmake ] && lang="$(sed -n 's/.*set(PROJECT_LANGUAGE[[:space:]]*"\{0,1\}\([A-Za-z]*\).*/\1/p' config.cmake | head -1)"
if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
  comp="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$main_bd/CMakeCache.txt" | head -1)"
  compflag="-DCMAKE_CXX_COMPILER=$comp"
else
  comp="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$main_bd/CMakeCache.txt" | head -1)"
  compflag="-DCMAKE_C_COMPILER=$comp"
fi
[ -n "$comp" ] || { echo "Could not read the compiler from $main_bd/CMakeCache.txt." >&2; exit 1; }
ccbase="$(basename "$comp")"

case "$main_bd" in build-*) sfx="${main_bd#build-}" ;; *) sfx="$ccbase" ;; esac
test_bd="test/build-${sfx}"
cov_arg=""; [ "$coverage" -eq 1 ] && cov_arg="-DP101_TEST_COVERAGE=ON"
compile_flag_arg="-DCMAKE_C_FLAGS="
if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
  compile_flag_arg="-DCMAKE_CXX_FLAGS="
fi
sanitizer_flags="$(sed -n 's/^DETECTED_SANITIZERS:STRING=//p' "$main_bd/CMakeCache.txt" | head -1)"
sanitizer_flags="${sanitizer_flags//;/ }"
sanitizer_args=()
if [ -n "$sanitizer_flags" ]; then
  sanitizer_args+=("-DCMAKE_EXE_LINKER_FLAGS=$sanitizer_flags")
fi

echo ">> configuring test tree ($test_bd) with $ccbase"
cmake -S test -B "$test_bd" "$compflag" "$compile_flag_arg" "${sanitizer_args[@]}" ${cov_arg:+$cov_arg} >/dev/null
echo ">> building tests"; cmake --build "$test_bd"
echo ">> ctest"; ( cd "$test_bd" && ctest --output-on-failure ${ctest_args[@]+"${ctest_args[@]}"} )
