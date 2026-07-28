#!/usr/bin/env bash
# coverage-report.sh — run the built target(s) and produce + open an HTML code
# coverage report (gcovr), using the gcov that matches the configured compiler.
# Reports coverage from BOTH the main build and, if present, the test build
# (test/build-<cc>) — so `./test.sh --coverage && ./coverage-report.sh -R`
# shows what your Unity tests exercised. Platforms: macOS, Linux, FreeBSD.
set -euo pipefail
CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

usage() {
  cat <<'USAGE'
Usage: ./coverage-report.sh [--report-only] [-- <program args>]
  Builds coverage-<compiler>/index.html with gcovr from the coverage data in the
  main build AND the test build (test/build-<cc>) if either exists.

  --min <pct>         fail (non-zero exit) if line coverage is below <pct>.
  --no-open           do not open the HTML report in a browser (hooks/CI).
  --report-only (-R)  don't run the main target(s); just report accumulated
                      .gcda (e.g. after ./test.sh --coverage, or several runs).

  Coverage build first:  ./change-compiler.sh -c <cc> --coverage && ./build.sh
  Test coverage:         ./test.sh --coverage
  gcovr required:  brew install gcovr  (macOS)  |  pipx install gcovr
USAGE
}
case " $* " in *" --help "*|*" -h "*) usage; exit 0 ;; esac

report_only=0
min_cov=""
no_open=0
prog_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --report-only|-R) report_only=1; shift ;;
    --min|--fail-under) min_cov="${2:?}"; shift 2 ;;
    --no-open)        no_open=1; shift ;;
    --)               shift; prog_args=("$@"); break ;;
    *) echo "Unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

main_bd="build"
[[ -f .last-build-dir ]] && main_bd="$(cat .last-build-dir)"
[[ -d "$main_bd" && -f "$main_bd/CMakeCache.txt" ]] || {
  echo "No configured build dir ('$main_bd'). Run:" >&2
  echo "  ./change-compiler.sh -c <cc> --coverage && ./build.sh" >&2
  exit 1
}

# suffix for output-dir naming
case "$main_bd" in build-*) _sfx="${main_bd#build-}" ;; *) _sfx="" ;; esac

has_gcno() { [[ -d "$1" && "$(find "$1" -name '*.gcno' 2>/dev/null | wc -l | tr -d '[:space:]')" != "0" ]]; }

# collect the dirs that actually hold coverage data: the main build plus any
# test build tree (test/build or test/build-<cc>) created by ./test.sh --coverage
cov_dirs=()
has_gcno "$main_bd" && cov_dirs+=("$main_bd")
if [[ -d test ]]; then
  shopt -s nullglob
  for _tbd in test/build test/build-*; do
    has_gcno "$_tbd" && cov_dirs+=("$_tbd")
  done
  shopt -u nullglob
fi
if [[ ${#cov_dirs[@]} -eq 0 ]]; then
  echo "No .gcno found in '$main_bd' or test build — build with coverage first:" >&2
  echo "  ./change-compiler.sh -c <cc> --coverage && ./build.sh   (and/or ./test.sh --coverage)" >&2
  exit 1
fi

command -v gcovr >/dev/null 2>&1 || {
  echo "gcovr not found. Install it:  brew install gcovr  (macOS)  |  pipx install gcovr" >&2
  exit 1
}

# gcov tool matching the configured compiler
cc="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$main_bd/CMakeCache.txt" | head -1)"
ccbase="$(basename "${cc:-cc}")"
case "$ccbase" in
  gcc-*)  gcov_tool="gcov-${ccbase#gcc-}" ;;
  *gcc*)  gcov_tool="gcov" ;;
  clang*) gcov_tool="llvm-cov gcov" ;;
  *)      gcov_tool="gcov" ;;
esac
if [[ "$gcov_tool" == "llvm-cov gcov" ]] && ! command -v llvm-cov >/dev/null 2>&1 \
   && command -v xcrun >/dev/null 2>&1; then gcov_tool="xcrun llvm-cov gcov"; fi
command -v "${gcov_tool%% *}" >/dev/null 2>&1 || { echo "warning: '${gcov_tool}' not found; using 'gcov'" >&2; gcov_tool="gcov"; }

# run main executable target(s) to generate main-build .gcda (unless report-only)
if [[ $report_only -eq 0 ]] && has_gcno "$main_bd"; then
  exes=()
  if [[ -f config.cmake ]]; then
    raw="$(tr '\n' ' ' < config.cmake | sed -n 's/.*set(EXECUTABLE_TARGETS\([^)]*\)).*/\1/p' | tr -d '"')"
    for t in $raw; do exes+=("$t"); done
  fi
  [[ ${#exes[@]} -gt 0 ]] || exes=(main)
  echo ">> running target(s) for coverage: ${exes[*]}"
  for t in "${exes[@]}"; do
    bin=""
    [[ -x "$main_bd/$t" ]] && bin="$main_bd/$t"
    if [[ -z "$bin" ]]; then
      while IFS= read -r f; do [[ -x "$f" ]] && { bin="$f"; break; }; done \
        < <(find "$main_bd" -type f -name "$t" 2>/dev/null)
    fi
    [[ -n "$bin" && -x "$bin" ]] || { echo "   (skip: no executable for '$t')"; continue; }
    echo "   $bin ${prog_args[*]-}"
    "$bin" ${prog_args[@]+"${prog_args[@]}"} || echo "   ($t exited non-zero; coverage still recorded)"
  done
else
  echo ">> --report-only or test-only: reporting accumulated .gcda"
fi

cov_out="coverage${_sfx:+-$_sfx}"
rm -rf "$cov_out"; mkdir -p "$cov_out"
echo ">> gcovr (gcov: $gcov_tool) over: ${cov_dirs[*]} -> $cov_out/"
gcovr --gcov-executable "$gcov_tool" -r . "${cov_dirs[@]}" --decisions --html-details -o "$cov_out/index.html"
gcovr --gcov-executable "$gcov_tool" -r . "${cov_dirs[@]}" --decisions > "$cov_out/summary.txt"
echo "---- summary ----"
cat "$cov_out/summary.txt"

if [[ $no_open -eq 0 ]]; then
  if   command -v open     >/dev/null 2>&1; then open "$cov_out/index.html" >/dev/null 2>&1 || true
  elif command -v xdg-open >/dev/null 2>&1; then xdg-open "$cov_out/index.html" >/dev/null 2>&1 || true
  fi
fi
echo "Report: $(pwd)/$cov_out/index.html"

# --- coverage gate (opt-in via --min) ------------------------------------
# gcovr --fail-under-line does the enforcing: exit 0 if line coverage >= the
# threshold, non-zero if under. We surface the measured % from --print-summary
# and turn it into a clear PASS/FAIL with a matching exit code (hook/CI ready).
if [[ -n "$min_cov" ]]; then
  set +e
  gate_out="$(gcovr --gcov-executable "$gcov_tool" -r . "${cov_dirs[@]}" --print-summary --fail-under-line "$min_cov" 2>&1)"
  gate_rc=$?
  set -e
  measured="$(printf '%s\n' "$gate_out" | grep -iE '^lines:' | grep -oE '[0-9]+(\.[0-9]+)?' | head -1)"
  _gl="======================================================================"
  echo; echo "$_gl"
  if [[ $gate_rc -eq 0 ]]; then
    echo " COVERAGE PASS — lines ${measured:+${measured}% }>= ${min_cov}% threshold."
    echo "$_gl"
  else
    echo " COVERAGE FAIL — lines ${measured:+${measured}% }< ${min_cov}% threshold." >&2
    echo "   open $cov_out/index.html to see the uncovered lines." >&2
    echo "$_gl" >&2
    exit 1
  fi
fi
