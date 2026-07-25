#!/usr/bin/env bash
# fuzz.sh — build and run a libFuzzer target with the sanitizers on. libFuzzer
# is coverage-guided and in-process; combined with ASan/UBSan it turns "no
# crash" into "no memory error or UB on this input". clang-only: needs a
# clang/clang++ that ships the fuzzer runtime (Homebrew LLVM / clang-NN — NOT
# Apple's /usr/bin/clang). C and C++, macOS / Linux / FreeBSD.
#
# Ends with an explicit PASS / FAIL verdict and exits 0 on pass, 1 on a finding,
# so it drops straight into a hook or CI step. Language is read from
# config.cmake (PROJECT_LANGUAGE), the same way test.sh does it.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

usage() {
  cat <<'USAGE'
Usage: ./fuzz.sh [-t <seconds>] [-- <extra libFuzzer args>]
  Builds fuzz/ with a fuzzer-capable clang/clang++ (+ASan/UBSan) and runs it
  against fuzz/corpus for a bounded time, then prints PASS or FAIL.
  -t <seconds>   wall-clock budget (default 30). 0 = run until stopped.
  -- <args>      pass through to the fuzzer (e.g. -- -runs=1000000 -jobs=4).
  Set FUZZ_CC=/path/to/clang(++) to force a specific compiler.
  --can-fuzz     print a usable compiler and exit 0, or exit 1 (no run).
  Crash reproducers (on FAIL) are written under fuzz/artifacts/.
USAGE
}
secs=30; extra=(); can_fuzz_only=0
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    -t) secs="${2:?}"; shift 2 ;;
    --can-fuzz) can_fuzz_only=1; shift ;;
    --) shift; extra=("$@"); break ;;
    *) echo "Unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

[ -f fuzz/CMakeLists.txt ] || { echo "No fuzz/ tree here." >&2; exit 1; }

# project language -> C vs C++ toolchain (same detection test.sh uses)
lang="C"; [ -f config.cmake ] && lang="$(sed -n 's/.*set(PROJECT_LANGUAGE[[:space:]]*"\{0,1\}\([A-Za-z]*\).*/\1/p' config.cmake | head -1)"
if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
  xlang="c++"; cc_var="CMAKE_CXX_COMPILER"
  cands="${FUZZ_CC:-} clang++-22 clang++-21 clang++ /opt/homebrew/opt/llvm/bin/clang++"
  probe_src='extern "C" int LLVMFuzzerTestOneInput(const unsigned char*d,unsigned long s){(void)d;(void)s;return 0;}'
else
  xlang="c";   cc_var="CMAKE_C_COMPILER"
  cands="${FUZZ_CC:-} clang-22 clang-21 clang /opt/homebrew/opt/llvm/bin/clang"
  probe_src='int LLVMFuzzerTestOneInput(const unsigned char*d,unsigned long s){(void)d;(void)s;return 0;}'
fi

# find a compiler that can actually link a libFuzzer target in this language
probe() {
  local cc="$1"
  command -v "$cc" >/dev/null 2>&1 || [ -x "$cc" ] || return 1
  printf '%s\n' "$probe_src" \
    | "$cc" -x "$xlang" -fsanitize=fuzzer -o /tmp/.p101_fzprobe.$$ - >/dev/null 2>&1 || return 1
  rm -f /tmp/.p101_fzprobe.$$; return 0
}
CC=""
for cand in $cands; do
  [ -n "$cand" ] || continue
  if probe "$cand"; then CC="$cand"; break; fi
done

# --can-fuzz: report only whether a fuzzer-capable compiler exists (used by
# check.sh to tell "no fuzzer installed" (skip) from "fuzzer found a bug").
if [ "$can_fuzz_only" -eq 1 ]; then
  if [ -n "$CC" ]; then echo "$CC"; exit 0; else exit 1; fi
fi
[ -n "$CC" ] || { echo "No fuzzer-capable ${xlang} compiler found. libFuzzer needs a clang/clang++ with the" >&2
  echo "fuzzer runtime (Homebrew LLVM, or clang-NN); Apple's /usr/bin/clang does not ship it. Try: brew install llvm" >&2; exit 1; }

echo ">> fuzzer compiler: $CC"
bd="fuzz/build-$(basename "$CC")"
cmake -S fuzz -B "$bd" -D"${cc_var}=$CC" >/dev/null
cmake --build "$bd"

bin="$bd/fuzz"
[ -x "$bin" ] || { echo "fuzz target not built ($bin)." >&2; exit 1; }
mkdir -p fuzz/corpus fuzz/findings fuzz/artifacts

# Marker so we can tell a defect found THIS run from stale artifacts.
start_marker="$bd/.last-run-start"
: > "$start_marker"

# libFuzzer writes newly-discovered inputs into the FIRST corpus dir. We give it
# fuzz/findings (transient) so the committed seed corpus (fuzz/corpus) stays clean.
echo ">> running: $bin fuzz/findings fuzz/corpus -max_total_time=$secs (findings -> fuzz/findings/, crashes -> fuzz/artifacts/)"
rc=0
"$bin" fuzz/findings fuzz/corpus -max_total_time="$secs" -print_final_stats=1 \
       -artifact_prefix=fuzz/artifacts/ ${extra[@]+"${extra[@]}"} || rc=$?

newart="$(find fuzz/artifacts -type f -newer "$start_marker" 2>/dev/null | head -1)"
rm -f "$start_marker"

echo
line="======================================================================"
if [ -n "$newart" ]; then
  echo "$line"
  echo " FAIL — the fuzzer found a defect (crash/leak/UB)."
  echo "   reproducer : $newart"
  echo "   replay it  : $bin $newart"
  echo "$line"
  exit 1
elif [ "$rc" -eq 0 ]; then
  echo "$line"
  echo " PASS — no crash, leak, or undefined behavior found in ${secs}s."
  echo "        (Absence of a finding in the time budget, not a proof of"
  echo "         correctness — run longer for more assurance.)"
  echo "$line"
  exit 0
else
  echo "$line"
  echo " INCOMPLETE — fuzzer stopped early (exit $rc) with no reproducer."
  echo "   Interrupted, or a run/setup error above — not a pass/fail verdict."
  echo "$line"
  exit "$rc"
fi
