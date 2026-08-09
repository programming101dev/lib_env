#!/usr/bin/env bash
# check.sh — the one-command quality gate. Runs the whole loop and prints ONE
# verdict: format check -> strict analysis build -> unit tests -> fuzz smoke,
# plus an OPT-IN coverage gate (--cov <pct>). Exits 0 only if every APPLICABLE
# step passes, non-zero otherwise, so it drops straight into a pre-submit hook
# or a CI step. macOS / Linux / FreeBSD; bash 3.2-safe.
#
# It orchestrates the existing scripts (build.sh, test.sh, fuzz.sh,
# coverage-report.sh) rather than re-implementing anything — one place to look,
# one green/red answer.
#
# NOTE: intentionally NOT `set -e`. We run each step, record pass/fail, and
# report them all so you can fix everything in one pass, not one-at-a-time.
set -uo pipefail
CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" || exit 1

fuzz_secs=20
do_fuzz=1
do_format=1
cov_min=""
quiet_build="-q"

usage() {
  cat <<'USAGE'
Usage: ./check.sh [-t <seconds>] [--cov <pct>] [--no-fuzz] [--no-format] [-v]
  Runs format-check, the strict build, unit tests, and a short fuzz smoke,
  then prints one PASS/FAIL verdict
  (non-zero exit on any failure).
  -t <seconds>   fuzz smoke budget (default 20).
  --cov <pct>    also run the tests with coverage and FAIL if line coverage is
                 below <pct> (requires gcovr and coverage-report.sh).
  --no-fuzz      skip the fuzz smoke step.
  --no-format    skip the format check.
  -v             verbose build (show the per-file compile commands).
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    -t) fuzz_secs="${2:?}"; shift 2 ;;
    --cov) cov_min="${2:?}"; shift 2 ;;
    --no-fuzz) do_fuzz=0; shift ;;
    --no-format) do_format=0; shift ;;
    -v) quiet_build=""; shift ;;
    *) echo "Unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

line="======================================================================"
fmt_st="SKIP"; build_st="SKIP"; test_st="SKIP"; fuzz_st="SKIP"; cov_st="SKIP (use --cov <pct>)"
failed=""
hdr() { echo; echo "$line"; echo ">>> $1"; echo "$line"; }

# 1) format check — fast, no build needed --------------------------------------
if [ "$do_format" -eq 1 ] && [ -x ./build.sh ]; then
  hdr "format check"
  if ./build.sh --format-check; then fmt_st="PASS"; else fmt_st="FAIL"; failed="$failed format"; fi
elif [ "$do_format" -eq 0 ]; then
  fmt_st="SKIP (--no-format)"
else
  fmt_st="FAIL (build.sh missing)"
  failed="$failed format"
fi

# 2) strict analysis build -----------------------------------------------------
if [ -x ./build.sh ]; then
  hdr "strict build"
  if ./build.sh $quiet_build; then build_st="PASS"; else build_st="FAIL"; failed="$failed build"; fi
else
  build_st="FAIL (build.sh missing)"
  failed="$failed build"
fi

# 3) unit tests — only meaningful if the build succeeded -----------------------
# With --cov we instrument the test tree so the coverage step has data to gate.
test_cmd="./test.sh"; [ -n "$cov_min" ] && test_cmd="./test.sh --coverage"
if [ -x ./test.sh ] && [ -d test ] && [ -f test/CMakeLists.txt ]; then
  if [ "$build_st" = "PASS" ]; then
    hdr "unit tests${cov_min:+ (with coverage)}"
    if $test_cmd; then test_st="PASS"; else test_st="FAIL"; failed="$failed tests"; fi
  else
    test_st="SKIP (build failed)"
  fi
else
  test_st="SKIP (no test/ tree)"
fi

# 4) fuzz smoke — skip cleanly if no fuzzer-capable clang is installed ----------
if [ "$do_fuzz" -eq 0 ]; then
  fuzz_st="SKIP (--no-fuzz)"
elif [ -x ./fuzz.sh ] && [ -f fuzz/CMakeLists.txt ]; then
  if ./fuzz.sh --can-fuzz >/dev/null 2>&1; then
    hdr "fuzz smoke (${fuzz_secs}s)"
    if ./fuzz.sh -t "$fuzz_secs"; then fuzz_st="PASS"; else fuzz_st="FAIL"; failed="$failed fuzz"; fi
  else
    fuzz_st="SKIP (no fuzzer-capable clang)"
  fi
else
  fuzz_st="SKIP (no fuzz/ tree)"
fi

# 5) coverage gate — opt-in via --cov <pct> ------------------------------------
if [ -n "$cov_min" ]; then
  if [ ! -x ./coverage-report.sh ]; then
    cov_st="FAIL (no coverage-report.sh)"
    failed="$failed coverage"
  elif ! command -v gcovr >/dev/null 2>&1; then
    cov_st="FAIL (gcovr not installed)"
    failed="$failed coverage"
  elif [ "$test_st" != "PASS" ]; then
    cov_st="FAIL (tests didn't pass)"
    failed="$failed coverage"
  else
    hdr "coverage gate (>= ${cov_min}% lines)"
    if ./coverage-report.sh --report-only --no-open --min "$cov_min"; then
      cov_st="PASS"
    else
      cov_st="FAIL"; failed="$failed coverage"
    fi
  fi
fi

# verdict ----------------------------------------------------------------------
echo
echo "$line"
printf ' %-8s : %s\n' "format"   "$fmt_st"
printf ' %-8s : %s\n' "build"    "$build_st"
printf ' %-8s : %s\n' "tests"    "$test_st"
printf ' %-8s : %s\n' "fuzz"     "$fuzz_st"
printf ' %-8s : %s\n' "coverage" "$cov_st"
echo "$line"
if [ -n "$failed" ]; then
  echo " CHECKS FAILED —$failed"
  echo "$line"
  exit 1
fi
echo " ALL CHECKS PASSED"
echo "$line"
exit 0
