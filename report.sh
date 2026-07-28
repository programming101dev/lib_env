#!/usr/bin/env bash
# report.sh — one entry point for this project's coverage and profiling reports.
set -euo pipefail
CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

usage() {
  cat <<'USAGE'
Usage: ./report.sh <coverage|profile> [options] [-- <program args>]

  coverage   run + gcovr HTML report in  coverage-<compiler>/index.html
             (aliases: cov, c)
  profile    run under a sampling profiler -> profile-<compiler>/
             macOS: Instruments; Linux: perf   (aliases: prof, p)

Options are passed through to the underlying script, e.g.:
  ./report.sh coverage -- -v -d 1        # run with args, then report coverage
  ./report.sh coverage --report-only     # report accumulated .gcda, no run
  ./report.sh profile  -- -v -d 1        # profile a run
USAGE
}

case " ${1-} " in " -h "|" --help "|"  ") usage; exit 0 ;; esac

sub="${1-}"; shift || true
case "$sub" in
  coverage|cov|c) exec bash ./coverage-report.sh "$@" ;;
  profile|prof|p) exec bash ./profile-report.sh "$@" ;;
  *) echo "Unknown subcommand: '${sub}'" >&2; usage; exit 2 ;;
esac
