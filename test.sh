#!/usr/bin/env bash
# test.sh — build and run this project's Unity test suite via ctest.
# Tests live in a SEPARATE CMake tree under test/, so vendored Unity and the
# test code are not subjected to the project's strict analysis build. Uses the
# same compiler the main build is using (from .last-build-dir). C and C++.
set -euo pipefail
CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

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

# An aggregate runner may require tests for a particular compiler even when a
# coverage or conformance pass most recently updated a different build marker.
lang="C"; [ -f config.cmake ] && lang="$(sed -n 's/.*set(PROJECT_LANGUAGE[[:space:]]*"\{0,1\}\([A-Za-z]*\).*/\1/p' config.cmake | head -1)"
requested_compiler="${P101_TEST_CC:-}"
if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
  requested_compiler="${P101_TEST_CXX:-}"
fi

p101_compiler_identity() {
  local path="$1"
  local target
  local directory

  case "$path" in
    */*) ;;
    *) path="$(command -v "$path" 2>/dev/null || printf '%s' "$path")" ;;
  esac
  while [ -L "$path" ]; do
    target="$(readlink "$path")"
    case "$target" in
      /*) path="$target" ;;
      *) path="$(dirname "$path")/$target" ;;
    esac
  done
  directory="$(CDPATH='' cd -P -- "$(dirname "$path")" 2>/dev/null && pwd -P)" \
    || { printf '%s' "$1"; return; }
  printf '%s/%s' "$directory" "$(basename "$path")"
}

p101_find_compiler_build() {
  local requested="$1" cache_key="$2" marker candidate cached requested_identity

  requested_identity="$(p101_compiler_identity "$requested")"

  for marker in .last-build-dir .last-runtime-build-dir; do
    if [ -f "$marker" ]; then
      candidate="$(cat "$marker")"
      if [ -f "$candidate/CMakeCache.txt" ]; then
        cached="$(sed -n "s/^${cache_key}:[^=]*=//p" "$candidate/CMakeCache.txt" | head -1)"
        [ "$(p101_compiler_identity "$cached")" != "$requested_identity" ] \
          || { printf '%s' "$candidate"; return 0; }
      fi
    fi
  done
  for candidate in build-*; do
    [ -f "$candidate/CMakeCache.txt" ] || continue
    cached="$(sed -n "s/^${cache_key}:[^=]*=//p" "$candidate/CMakeCache.txt" | head -1)"
    [ "$(p101_compiler_identity "$cached")" != "$requested_identity" ] \
      || { printf '%s' "$candidate"; return 0; }
  done
  return 1
}

main_bd="build"
if [ -n "$requested_compiler" ]; then
  cache_key="CMAKE_C_COMPILER"
  if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
    cache_key="CMAKE_CXX_COMPILER"
  fi
  main_bd="$(p101_find_compiler_build "$requested_compiler" "$cache_key")" || {
    echo "No configured main build uses requested compiler '$requested_compiler'." >&2
    exit 1
  }
elif [ -f .last-build-dir ]; then
  main_bd="$(cat .last-build-dir)"
elif [ -f .last-runtime-build-dir ]; then
  main_bd="$(cat .last-runtime-build-dir)"
fi
[ -f "$main_bd/CMakeCache.txt" ] || { echo "No configured main build ('$main_bd'). Run ./change-compiler.sh first." >&2; exit 1; }

# project language decides which compiler the test tree needs
if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
  comp="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$main_bd/CMakeCache.txt" | head -1)"
  compflag="-DCMAKE_CXX_COMPILER=$comp"
else
  comp="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$main_bd/CMakeCache.txt" | head -1)"
  compflag="-DCMAKE_C_COMPILER=$comp"
fi
[ -n "$comp" ] || { echo "Could not read the compiler from $main_bd/CMakeCache.txt." >&2; exit 1; }
ccbase="$(basename "$comp")"

p101_companion_compiler() {
  local compiler_dir compiler_name candidate_name candidate

  compiler_dir="$(dirname "$1")"
  compiler_name="$(basename "$1")"
  case "$compiler_name" in
    clang++) candidate_name="clang" ;;
    clang++-*) candidate_name="clang-${compiler_name#clang++-}" ;;
    clang) candidate_name="clang++" ;;
    clang-*) candidate_name="clang++-${compiler_name#clang-}" ;;
    g++) candidate_name="gcc" ;;
    g++-*) candidate_name="gcc-${compiler_name#g++-}" ;;
    gcc) candidate_name="g++" ;;
    gcc-*) candidate_name="g++-${compiler_name#gcc-}" ;;
    c++) candidate_name="cc" ;;
    cc) candidate_name="c++" ;;
    *) return 1 ;;
  esac
  candidate="$compiler_dir/$candidate_name"
  if [ -x "$candidate" ]; then
    printf '%s' "$candidate"
    return 0
  fi
  command -v "$candidate_name" 2>/dev/null
}

# Most library test trees deliberately compile both their C API and a C++
# header-compatibility target. Pair clang with clang++ (or gcc with g++) so
# sanitizer flags probed for one compiler are never handed to an unrelated
# default compiler selected by CMake.
companion_args=()
companion_comp=""
test_project_line="$(sed -n '/^[[:space:]]*project[[:space:]]*(/p' test/CMakeLists.txt | head -1)"
if companion_comp="$(p101_companion_compiler "$comp")"; then
  if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
    if printf '%s\n' "$test_project_line" | grep -Eq '(^|[[:space:](])C([[:space:])]|$)'; then
      companion_args+=("-DCMAKE_C_COMPILER=$companion_comp")
    fi
  elif printf '%s\n' "$test_project_line" | grep -Eq '(^|[[:space:](])CXX([[:space:])]|$)'; then
    companion_args+=("-DCMAKE_CXX_COMPILER=$companion_comp")
  fi
fi

case "$main_bd" in build-*) sfx="${main_bd#build-}" ;; *) sfx="$ccbase" ;; esac
test_bd="test/build-${sfx}"
if [ "$coverage" -eq 1 ]; then
  # Coverage must never retain objects for sources that were removed from the
  # test target; CMake's incremental clean rules no longer know about them.
  rm -rf "$test_bd"
fi
expected_test_source="$(CDPATH='' cd test && pwd)"
if [ -f "$test_bd/CMakeCache.txt" ]; then
  cached_test_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$test_bd/CMakeCache.txt" | head -1)"
  if [ -n "$cached_test_source" ] && [ "$cached_test_source" != "$expected_test_source" ]; then
    echo ">> removing test cache moved from $cached_test_source"
    rm -rf "$test_bd"
  fi
fi
if [ -f "$test_bd/CMakeCache.txt" ]; then
  if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
    cached_test_compiler="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$test_bd/CMakeCache.txt" | head -1)"
  else
    cached_test_compiler="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$test_bd/CMakeCache.txt" | head -1)"
  fi
  if [ -n "$cached_test_compiler" ] && [ "$cached_test_compiler" != "$comp" ]; then
    echo ">> removing test cache configured for $cached_test_compiler"
    rm -rf "$test_bd"
  fi
fi
if [ -f "$test_bd/CMakeCache.txt" ] && [ "${#companion_args[@]}" -ne 0 ]; then
  if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
    cached_companion_compiler="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$test_bd/CMakeCache.txt" | head -1)"
  else
    cached_companion_compiler="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$test_bd/CMakeCache.txt" | head -1)"
  fi
  if [ -n "$cached_companion_compiler" ] && [ "$cached_companion_compiler" != "$companion_comp" ]; then
    echo ">> removing test cache configured for companion $cached_companion_compiler"
    rm -rf "$test_bd"
  fi
fi
cov_arg="-DP101_TEST_COVERAGE=OFF"
[ "$coverage" -eq 1 ] && cov_arg="-DP101_TEST_COVERAGE=ON"
sanitizer_flags=""
if [ "$coverage" -eq 0 ]; then
  sanitizer_flags="$(sed -n 's/^DETECTED_SANITIZERS:STRING=//p' "$main_bd/CMakeCache.txt" | head -1)"
  sanitizer_flags="${sanitizer_flags//;/ }"
fi
compile_flag_arg="-DCMAKE_C_FLAGS=$sanitizer_flags"
if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
  compile_flag_arg="-DCMAKE_CXX_FLAGS=$sanitizer_flags"
fi
sanitizer_args=()
if [ -n "$sanitizer_flags" ]; then
  sanitizer_args+=("-DCMAKE_EXE_LINKER_FLAGS=$sanitizer_flags")
fi


p101_preferred_build_dir="build-$ccbase"
p101_path_args=()
p101_join_paths() {
  local out="" path
  for path in "$@"; do
    if [ -z "$out" ]; then out="$path"; else out="$out $path"; fi
  done
  printf '%s' "$out"
}
p101_find_workspace_root() {
  local dir="$PWD"
  while [ "$dir" != "/" ]; do
    if [ -d "$dir/libraries" ] && [ -f "$dir/scripts/repos.txt" ]; then
      printf '%s' "$dir"
      return 0
    fi
    dir="$(dirname "$dir")"
  done
  return 1
}
if p101_workspace_root="$(p101_find_workspace_root)"; then
  p101_local_include_dirs=()
  p101_local_link_dirs=()
  for inc in "$p101_workspace_root"/libraries/*/include; do
    [ -d "$inc" ] && p101_local_include_dirs+=("$inc")
  done
  for lib in "$p101_workspace_root"/libraries/*; do
    [ -d "$lib" ] || continue
    [ -d "$lib/$p101_preferred_build_dir" ] && p101_local_link_dirs+=("$lib/$p101_preferred_build_dir")
    if [ -f "$lib/.last-build-dir" ]; then
      p101_last_build_dir="$(cat "$lib/.last-build-dir")"
      [ -d "$lib/$p101_last_build_dir" ] && p101_local_link_dirs+=("$lib/$p101_last_build_dir")
    fi
    for p101_build_dir in "$lib"/build*; do
      [ -d "$p101_build_dir" ] && p101_local_link_dirs+=("$p101_build_dir")
    done
  done
  p101_local_include_dirs_joined="$(p101_join_paths ${p101_local_include_dirs[@]+"${p101_local_include_dirs[@]}"})"
  p101_local_link_dirs_joined="$(p101_join_paths ${p101_local_link_dirs[@]+"${p101_local_link_dirs[@]}"})"
  [ -n "$p101_local_include_dirs_joined" ] && p101_path_args+=("-DP101_PUBLIC_INCLUDE_DIRS=$p101_local_include_dirs_joined")
  [ -n "$p101_local_link_dirs_joined" ] && p101_path_args+=("-DP101_PUBLIC_LINK_DIRS=$p101_local_link_dirs_joined")
fi

echo ">> configuring test tree ($test_bd) with $ccbase"
cmake -S test -B "$test_bd" "$compflag" ${companion_args[@]+"${companion_args[@]}"} \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$compile_flag_arg" \
  ${sanitizer_args[@]+"${sanitizer_args[@]}"} \
  ${p101_path_args[@]+"${p101_path_args[@]}"} "$cov_arg" >/dev/null
if [ "$coverage" -eq 1 ]; then
  # A source edit can change gcov's counter layout without changing the .gcda
  # filename. Never merge a new test run into stale runtime data.
  find "$test_bd" -type f -name '*.gcda' -exec rm -f {} +
fi
echo ">> building tests"; cmake --build "$test_bd"
echo ">> ctest"; ( cd "$test_bd" && ctest --output-on-failure ${ctest_args[@]+"${ctest_args[@]}"} )
