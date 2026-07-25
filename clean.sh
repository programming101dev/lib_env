#!/usr/bin/env bash
# clean.sh — remove ALL of this project's transient output for a clean slate:
# every build tree (main, test, fuzz, debug), the coverage/profile output, the
# fuzzer's crash artifacts, and the generated compile_commands.json symlink.
# Never touches source (src/, include/, test/*.c, config.cmake, *.sh) or the
# committed fuzz corpus (fuzz/corpus/). Safe to re-run.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

usage() {
  cat <<'USAGE'
Usage: ./clean.sh [-n|--dry-run]
  Removes generated output only:
    build/ build-<cc>/ cmake-build-*/          (main build trees)
    test/build/ test/build-<cc>/               (unit-test build trees)
    fuzz/build-<cc>/ fuzz/artifacts/ fuzz/findings/  (fuzzer builds, crashes, discovered corpus)
    debug/ debug-<cc>/                         (debug.sh builds)
    coverage/ coverage-<cc>/ profile/ profile-<cc>/
    .last-build-dir  compile_commands.json     (state + generated symlink)
    stray *.gcda *.gcno *.gcov *.su *.ci gmon.out perf.data coverage.html
  Source, scripts, and the committed fuzz corpus (fuzz/corpus/) are never
  touched. Safe to re-run.
  -n, --dry-run   list what would be removed; delete nothing.
USAGE
}
case " $* " in *" --help "*|*" -h "*) usage; exit 0 ;; esac

dry=0
if [[ "${1-}" == "-n" || "${1-}" == "--dry-run" ]]; then dry=1; fi

shopt -s nullglob

dirs=()
for d in build build-* cmake-build-* \
         test/build test/build-* \
         fuzz/build-* fuzz/artifacts fuzz/findings \
         debug debug-* \
         coverage coverage-* profile profile-*; do
  if [[ -d "$d" ]]; then dirs+=("$d"); fi
done

files=()
for f in .last-build-dir compile_commands.json gmon.out perf.data coverage.html \
         *.gcda *.gcno *.gcov *.su *.ci; do
  # -L catches the (possibly dangling) compile_commands.json symlink too.
  if [[ -f "$f" || -L "$f" ]]; then files+=("$f"); fi
done

targets=( ${dirs[@]+"${dirs[@]}"} ${files[@]+"${files[@]}"} )
if [[ ${#targets[@]} -eq 0 ]]; then
  echo "Already clean."
  exit 0
fi

echo "Removing:"
printf '  %s\n' "${targets[@]}"
if [[ $dry -eq 1 ]]; then
  echo "(dry-run — nothing deleted)"
  exit 0
fi
rm -rf -- "${targets[@]}"
echo "Clean."
