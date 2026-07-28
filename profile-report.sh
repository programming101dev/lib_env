#!/usr/bin/env bash
# profile-report.sh — run the built target under a sampling profiler and show
# the result. macOS -> Instruments (Time Profiler) via xctrace, else `sample`;
# other platforms -> perf. Sampling needs no -pg, so any build works.
set -euo pipefail
CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

usage() {
  cat <<'USAGE'
Usage: ./profile-report.sh [-- <program args>]
  Runs the executable target from the last build under a sampling profiler and
  opens the result. No profiling build flag is needed — sampling profiles any
  build. Pass runtime arguments after --, e.g.:
    ./profile-report.sh -- -v -d 1

  macOS: `xctrace` (Instruments Time Profiler); falls back to `sample`.
  Linux: `perf record -g` then `perf report`.
  NOTE: a program that finishes in milliseconds yields no samples — give it a
  real workload (a loop / large input) to get a meaningful profile.
USAGE
}
case " $* " in *" --help "*|*" -h "*) usage; exit 0 ;; esac

prog_args=()
if [[ "${1-}" == "--" ]]; then shift; prog_args=("$@"); fi

build_dir="build"
[[ -f .last-build-dir ]] && build_dir="$(cat .last-build-dir)"
[[ -d "$build_dir" && -f "$build_dir/CMakeCache.txt" ]] || {
  echo "No configured build dir ('$build_dir'). Run: ./change-compiler.sh -c <cc> && ./build.sh" >&2
  exit 1
}

# parallel output dir next to the build dir:  build-gcc-16 -> profile-gcc-16
case "$build_dir" in build-*) _sfx="${build_dir#build-}" ;; *) _sfx="" ;; esac
prof_dir="profile${_sfx:+-$_sfx}"
rm -rf "$prof_dir"; mkdir -p "$prof_dir"

# first executable target from config.cmake (fallback: main)
exes=()
if [[ -f config.cmake ]]; then
  raw="$(tr '\n' ' ' < config.cmake | sed -n 's/.*set(EXECUTABLE_TARGETS\([^)]*\)).*/\1/p' | tr -d '"')"
  for t in $raw; do exes+=("$t"); done
fi
[[ ${#exes[@]} -gt 0 ]] || exes=(main)
t="${exes[0]}"

bin=""
[[ -x "$build_dir/$t" ]] && bin="$build_dir/$t"
if [[ -z "$bin" ]]; then
  while IFS= read -r f; do [[ -x "$f" ]] && { bin="$f"; break; }; done \
    < <(find "$build_dir" -type f -name "$t" 2>/dev/null)
fi
[[ -n "$bin" && -x "$bin" ]] || { echo "No executable found for target '$t' in '$build_dir'." >&2; exit 1; }
bin="$(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")"   # absolute path

os="$(uname -s)"
if [[ "$os" == "Darwin" ]]; then
  if command -v xctrace >/dev/null 2>&1; then
    out="$prof_dir/profile.trace"
    echo ">> Instruments Time Profiler (xctrace) -> $out"
    xctrace record --template 'Time Profiler' --output "$out" \
      --launch -- "$bin" ${prog_args[@]+"${prog_args[@]}"} || true
    if [[ -e "$out" ]]; then echo "Opening $out in Instruments"; open "$out" >/dev/null 2>&1 || true
    else echo "No trace produced (program too short?)." >&2; fi
  elif command -v sample >/dev/null 2>&1; then
    echo ">> sample (xctrace not found; install Xcode for Instruments)"
    "$bin" ${prog_args[@]+"${prog_args[@]}"} & pid=$!
    sample "$pid" 10 -mayDie -f "$prof_dir/profile.sample.txt" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "Report: $(pwd)/$prof_dir/profile.sample.txt"
    open "$prof_dir/profile.sample.txt" 2>/dev/null || true
  else
    echo "Neither xctrace nor sample found (install Xcode / Command Line Tools)." >&2
    exit 1
  fi
else
  if ! command -v perf >/dev/null 2>&1; then
    echo "perf not found. Install it, e.g.:" >&2
    echo "  sudo apt install linux-tools-common linux-tools-\$(uname -r)" >&2
    echo "(On FreeBSD, use 'pmcstat' or 'dtrace' instead.)" >&2
    exit 1
  fi
  echo ">> perf record -g -- $bin ${prog_args[*]-}"
  perf record -g -o "$prof_dir/perf.data" -- "$bin" ${prog_args[@]+"${prog_args[@]}"}
  echo ">> perf report  (interactive: q to quit; text copy -> $prof_dir/profile.perf.txt)"
  perf report -i "$prof_dir/perf.data" --stdio > "$prof_dir/profile.perf.txt" 2>/dev/null || true
  perf report -i "$prof_dir/perf.data" || true
fi
