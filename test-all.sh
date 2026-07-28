#!/usr/bin/env bash
# test-all.sh — run the Unity test suite across every supported compiler
# (mirrors build-all.sh). For each supported compiler it configures the main
# build (./change-compiler.sh) then runs ./test.sh, and tallies pass/fail.
set -uo pipefail
CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

usage() {
  cat <<'USAGE'
Usage: ./test-all.sh [--coverage]
  Runs ./test.sh against every compiler in supported_c_compilers.txt
  (or supported_cxx_compilers.txt for a C++ project). Exits non-zero if any
  compiler's suite fails.
  --coverage   pass --coverage through to each ./test.sh run.
USAGE
}
case " $* " in *" --help "*|*" -h "*) usage; exit 0 ;; esac
cov=""; [ "${1-}" = "--coverage" ] && cov="--coverage"

[ -f test.sh ] && [ -d test ] || { echo "No test tree here (test/ + test.sh)." >&2; exit 1; }

find_list() { for c in . scripts; do [ -f "$c/$1" ] && { echo "$c/$1"; return; }; done; }
names_from() { [ -f "$1" ] && awk 'NF && $0!~/^[[:space:]]*#/{n=split($0,a,"/");print a[n]}' "$1"; }

lang="C"
[ -f config.cmake ] && lang="$(sed -n 's/.*set(PROJECT_LANGUAGE[[:space:]]*"\{0,1\}\([A-Za-z]*\).*/\1/p' config.cmake | head -1)"
if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
  clist="$(names_from "$(find_list supported_cxx_compilers.txt)")"
  xlist="$(names_from "$(find_list supported_cxx_compilers.txt)")"
else
  clist="$(names_from "$(find_list supported_c_compilers.txt)")"
  xlist="$(names_from "$(find_list supported_cxx_compilers.txt)")"
fi
[ -n "$clist" ] || { echo "No supported compiler list found." >&2; exit 1; }

# pair C compilers with their C++ partner by position (for -x)
set -- $xlist; xarr="$*"
i=0; pass=0; fail=0; failed=""
for cc in $clist; do
  i=$((i+1))
  cxx="$(echo $xarr | cut -d' ' -f$i)"
  echo "==================================================================="
  echo ">> compiler: $cc${cxx:+  (c++: $cxx)}"
  echo "==================================================================="
  if ! command -v "$cc" >/dev/null 2>&1; then echo "   (skip: $cc not on PATH)"; continue; fi
  if ./change-compiler.sh -c "$cc" ${cxx:+-x "$cxx"} >/dev/null 2>&1; then
    if ./test.sh ${cov:+$cov}; then pass=$((pass+1)); else fail=$((fail+1)); failed="$failed $cc"; fi
  else
    echo "   (configure failed for $cc)"; fail=$((fail+1)); failed="$failed $cc"
  fi
done

echo "==================================================================="
printf 'test-all: %d passed, %d failed%s\n' "$pass" "$fail" "${failed:+ (failed:$failed)}"
[ "$fail" -eq 0 ]
