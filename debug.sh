#!/usr/bin/env bash
# debug.sh — the "a tool found a bug, now go look at it" companion to check.sh /
# fuzz.sh. It builds a clean -O0 -g, SANITIZER-FREE binary and then:
#
#   ./debug.sh                 launch the program under lldb / gdb
#   ./debug.sh -- -d 5         ... passing program arguments after --
#   ./debug.sh -v              run it under valgrind (memcheck; Linux / FreeBSD)
#   ./debug.sh -r <crashfile>  replay a fuzz/artifacts/crash-… under the debugger
#
# macOS / FreeBSD use lldb, Linux uses gdb (whichever is installed). The debug
# build is sanitizer-free and unoptimized on purpose, so valgrind works and
# stepping is clean — run ./check.sh / ./fuzz.sh for the sanitizer-instrumented
# builds. Sources, defines, standard and libraries all come from config.cmake.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

usage() {
  cat <<'USAGE'
Usage: ./debug.sh [-v] [-r <crashfile>] [-- <program args>]
  (no options)     build a -O0 -g binary and launch it under lldb / gdb
  -v, --valgrind   build and run under valgrind (memcheck; Linux / FreeBSD)
  -r <file>        replay a fuzzer crash reproducer under the debugger
  -- <args>        pass the rest as arguments to the program
Common debugger commands once it opens: run   (start),  bt  (backtrace on crash).
USAGE
}

mode="debug"; crash=""; prog_args=()
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    -v|--valgrind) mode="valgrind"; shift ;;
    -r|--replay) mode="replay"; crash="${2:?-r needs a crash file}"; shift 2 ;;
    --) shift; prog_args=("$@"); break ;;
    *) echo "Unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

[ -f config.cmake ] || { echo "No config.cmake here." >&2; exit 1; }
os="$(uname -s)"
lang="C"; lang="$(sed -n 's/.*set(PROJECT_LANGUAGE[[:space:]]*"\{0,1\}\([A-Za-z]*\).*/\1/p' config.cmake | head -1)"

pick_debugger() {
  local order d
  if [ "$os" = "Darwin" ]; then order=(lldb gdb); else order=(gdb lldb); fi
  for d in "${order[@]}"; do command -v "$d" >/dev/null 2>&1 && { echo "$d"; return; }; done
}

# ---- replay a fuzzer crash: reuse the instrumented fuzz binary --------------
if [ "$mode" = "replay" ]; then
  [ -f "$crash" ] || { echo "Crash file not found: $crash" >&2; exit 1; }
  # shellcheck disable=SC2012  # controlled names (build-<cc>/fuzz); -t picks the newest
  fbin="$(ls -t fuzz/build-*/fuzz 2>/dev/null | head -1 || true)"
  if [ -z "$fbin" ] || [ ! -x "$fbin" ]; then echo "No fuzz binary built — run ./fuzz.sh first." >&2; exit 1; fi
  dbg="$(pick_debugger)"; [ -n "$dbg" ] || { echo "No debugger (lldb/gdb) found." >&2; exit 1; }
  echo ">> replaying '$crash' under $dbg (type: run, then bt)"
  case "$dbg" in
    gdb)  exec gdb --args "$fbin" "$crash" ;;
    lldb) exec lldb -- "$fbin" "$crash" ;;
  esac
fi

# ---- read the first executable target out of config.cmake ------------------
extract_set() {  # $1 = set() variable name -> tokens, one per line (comments stripped)
  awk -v name="$1" '
    { line=$0
      if (!inblk) { i=index(line,"set(" name); if(i==0) next; inblk=1; line=substr(line,i+length("set(" name)) }
      c=index(line,")"); if(c>0){seg=substr(line,1,c-1);done=1}else{seg=line;done=0}
      h=index(seg,"#"); if(h>0) seg=substr(seg,1,h-1)
      n=split(seg,a,/[ \t\r]+/); for(k=1;k<=n;k++) if(a[k]!="") print a[k]
      if(done) exit }' config.cmake
}

target="$(extract_set EXECUTABLE_TARGETS | head -1)"
[ -n "$target" ] || { echo "No EXECUTABLE_TARGETS in config.cmake (nothing to debug)." >&2; exit 1; }

srcs=();     while IFS= read -r t; do [ -n "$t" ] && srcs+=("$t"); done < <(extract_set "${target}_SOURCES")
libs=();     while IFS= read -r t; do [ -n "$t" ] && libs+=("$t"); done < <(extract_set "${target}_LINK_LIBRARIES")
defs=();     while IFS= read -r t; do case "$t" in -D*|-U*) defs+=("$t") ;; esac; done < <(extract_set STANDARD_FLAGS)
[ "${#srcs[@]}" -gt 0 ] || { echo "config.cmake lists no sources for '$target'." >&2; exit 1; }

# language standard from config.cmake
if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
  std="$(extract_set CMAKE_CXX_STANDARD | head -1)"; stdflag=""; [ -n "$std" ] && stdflag="-std=c++$std"
else
  std="$(extract_set CMAKE_C_STANDARD | head -1)"; stdflag=""; [ -n "$std" ] && stdflag="-std=c$std"
fi

# compiler: prefer the one the main build was configured with, else a default
cc=""
if [ -f .last-build-dir ] && [ -f "$(cat .last-build-dir)/CMakeCache.txt" ]; then
  bd="$(cat .last-build-dir)"
  if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
    cc="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$bd/CMakeCache.txt" | head -1)"
  else
    cc="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$bd/CMakeCache.txt" | head -1)"
  fi
fi
if [ -z "$cc" ]; then
  if [ "$lang" = "CXX" ] || [ "$lang" = "CPP" ]; then
    for c in clang++ g++ c++; do command -v "$c" >/dev/null 2>&1 && { cc="$c"; break; }; done
  else
    for c in clang gcc cc; do command -v "$c" >/dev/null 2>&1 && { cc="$c"; break; }; done
  fi
fi
[ -n "$cc" ] || { echo "No compiler found." >&2; exit 1; }

# p101 libs install to /usr/local or /opt/homebrew (or /opt/local); search all.
incs=(-Iinclude -I/usr/local/include -I/opt/homebrew/include -I/opt/local/include)
libdirs=(-L/usr/local/lib -L/usr/local/lib64 -L/opt/homebrew/lib -L/opt/local/lib)
# shellcheck disable=SC2054  # commas are part of the -Wl linker flag, not array separators
rpaths=(-Wl,-rpath,/usr/local/lib -Wl,-rpath,/opt/homebrew/lib)
libflags=(); for l in ${libs[@]+"${libs[@]}"}; do libflags+=("-l$l"); done

outdir="debug-$(basename "$cc")"; mkdir -p "$outdir"
outbin="$outdir/$target"
echo ">> compiling '$target' (-O0 -g, sanitizer-free) with $(basename "$cc") -> $outbin"
"$cc" ${stdflag:+"$stdflag"} -O0 -g \
  ${defs[@]+"${defs[@]}"} "${incs[@]}" "${srcs[@]}" \
  "${libdirs[@]}" "${rpaths[@]}" ${libflags[@]+"${libflags[@]}"} -o "$outbin" \
  || { echo "debug build failed." >&2; exit 1; }

# ---- run: valgrind or the debugger -----------------------------------------
if [ "$mode" = "valgrind" ]; then
  if [ "$os" = "Darwin" ]; then
    echo "valgrind is not available on macOS. Use the sanitizer build instead: ./check.sh (ASan/UBSan)." >&2
    exit 1
  fi
  command -v valgrind >/dev/null 2>&1 || { echo "valgrind not installed (run the p101 setup)." >&2; exit 1; }
  echo ">> running under valgrind (leak-check + track-origins)"
  exec valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 \
       "./$outbin" ${prog_args[@]+"${prog_args[@]}"}
fi

dbg="$(pick_debugger)"; [ -n "$dbg" ] || { echo "No debugger (lldb/gdb) found." >&2; exit 1; }
echo ">> launching '$target' under $dbg (type: run, then bt after a crash)"
case "$dbg" in
  gdb)  exec gdb --args "./$outbin" ${prog_args[@]+"${prog_args[@]}"} ;;
  lldb) exec lldb -- "./$outbin" ${prog_args[@]+"${prog_args[@]}"} ;;
esac
