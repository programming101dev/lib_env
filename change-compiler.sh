#!/usr/bin/env bash
# change-compiler.sh — configure a C project with a chosen compiler & tools
# Supports: macOS, modern Linux, modern FreeBSD
set -euo pipefail

# --- opt-in coverage / profiling (P101) ---------------------------------
# Pull the long flags out before the normal option parser and export them.
# The shared CMakeLists reads P101_COVERAGE / P101_PROFILE at configure time
# and instruments the compile + link. Absent => nothing changes. If a parent
# (e.g. update.sh / build-all.sh) already exported them, they are inherited.
_p101_argv=()
for _p101_a in "$@"; do
  case "$_p101_a" in
    --coverage) export P101_COVERAGE=1 ;;
    --profile)  export P101_PROFILE=1 ;;
    *)          _p101_argv+=("$_p101_a") ;;
  esac
done
if ((${#_p101_argv[@]})); then set -- "${_p101_argv[@]}"; else set --; fi
unset _p101_argv _p101_a
# QoL: a bare first argument is taken as the C compiler, i.e.
#   ./change-compiler.sh gcc-16   ==   ./change-compiler.sh -c gcc-16
if [[ "${1-}" != "" && "${1-}" != -* ]]; then set -- -c "$@"; fi
# ------------------------------------------------------------------------

# ----------------- defaults -----------------
c_compiler=""
clang_format_name="clang-format"
clang_tidy_name="clang-tidy"
cppcheck_name="cppcheck"
sanitizers=""
sanitizers_passed=false

# Build dir behavior:
# - default is "build-<compiler-basename>" (e.g., build-clang, build-gcc-15)
# - override with -b <dir>
build_dir=""          # empty means auto
generator=""          # e.g. "Ninja" or "Unix Makefiles"
reuse_build=false     # -R=reuse build dir instead of wiping it
extra_cmake_args=()   # additional -Dfoo=bar etc.

# ----------------- usage -----------------
usage() {
  cat <<'USAGE' >&2
Usage: change-compiler.sh -c <cc> [-f <clang-format>] [-t <clang-tidy>] [-k <cppcheck>] [-s <sanitizers>] [-b <build-dir>] [-G <generator>] [-R] [--coverage] [--profile] [-- -D...]
  -c <cc>           C compiler (e.g. gcc, gcc-15, clang, /usr/bin/clang-18)
  -f <name>         clang-format executable (default: clang-format)
  -t <name>         clang-tidy executable   (default: clang-tidy)
  -k <name>         cppcheck executable     (default: cppcheck)
  -s <list>         comma list of sanitizers (e.g. address,undefined)
                    If omitted, reads sanitizers.txt (if present), else none.
  --coverage        instrument this build for code coverage (gcov)
  --profile         instrument this build for profiling (gprof)
  -b <dir>          build directory (default: build-<compiler>)
  -G <gen>          CMake generator (e.g. Ninja, "Unix Makefiles")
  -R                reuse existing build dir (do NOT delete it)
  --                pass remaining args straight to CMake (e.g., -DVAR=ON)

Examples:
  ./change-compiler.sh -c clang
  ./change-compiler.sh -c gcc-15 -s address,undefined -G Ninja
  ./change-compiler.sh -c /usr/bin/clang-18
  ./change-compiler.sh -c clang -b build-debug -- -DP101_STRICT=ON
USAGE
  exit 1
}

# --help / -h -> usage, exit 0 (P101 uniform CLI help)
case " $* " in *" --help "*|*" -h "*) ( usage ) || true; exit 0 ;; esac

# ----------------- args -----------------
while (( "$#" )); do
  case "$1" in
    -c) c_compiler="${2-}"; shift 2 ;;
    -f) clang_format_name="${2-}"; shift 2 ;;
    -t) clang_tidy_name="${2-}"; shift 2 ;;
    -k) cppcheck_name="${2-}"; shift 2 ;;
    -s) sanitizers="${2-}"; sanitizers_passed=true; shift 2 ;;
    -b) build_dir="${2-}"; shift 2 ;;
    -G) generator="${2-}"; shift 2 ;;
    -R) reuse_build=true; shift ;;
    --) shift; extra_cmake_args+=("$@"); break ;;
    -h|--help) usage ;;
    *) echo "Unknown option: $1" >&2; usage ;;
  esac
done

# ----------------- validation -----------------
[[ -n "$c_compiler" ]] || { echo "Error: -c <cc> is required." >&2; usage; }

# Resolve a tool to an absolute path.
# - accepts absolute paths
# - otherwise resolves via PATH
must_find() {
  local tool="$1"
  if [[ "$tool" = /* ]]; then
    [[ -x "$tool" ]] || { echo "Error: '$tool' not executable" >&2; exit 2; }
    printf '%s\n' "$tool"
    return 0
  fi

  local found=""
  found="$(command -v "$tool" 2>/dev/null || true)"
  [[ -n "$found" ]] || { echo "Error: '$tool' not found in PATH" >&2; exit 2; }
  printf '%s\n' "$found"
}

CC_PATH="$(must_find "$c_compiler")"
CLANG_FORMAT_PATH="$(must_find "$clang_format_name")"
CLANG_TIDY_PATH="$(must_find "$clang_tidy_name")"
CPPCHECK_PATH="$(must_find "$cppcheck_name")"

# ----------------- derive build dir (default: build-<compiler-basename>) -----------------
# Use basename of resolved compiler path (portable across macOS/Linux/FreeBSD).
if [[ -z "${build_dir}" ]]; then
  cc_base="$(basename "$CC_PATH")"
  build_dir="build-${cc_base}"
fi

# The build directory is recursively removed unless -R is used. Keep that
# destructive operation inside this project and limited to conventional CMake
# build-directory names.
case "$build_dir" in
  build|build-*|cmake-build-*) ;;
  *)
    echo "Error: unsafe build directory '$build_dir'." >&2
    echo "Use a top-level name matching build, build-*, or cmake-build-*." >&2
    exit 2 ;;
esac
if [[ -L "$build_dir" ]]; then
  echo "Error: refusing to use symlink as build directory: $build_dir" >&2
  exit 2
fi

# ----------------- sanitizers -----------------
if ! $sanitizers_passed; then
  if [[ -f "sanitizers.txt" ]]; then
    # Strip comments and whitespace; portable sed/tr for macOS/BSD/Linux.
    sanitizers="$(sed 's/#.*$//g' sanitizers.txt | tr -d '[:space:]')"
    echo "Sanitizers loaded from sanitizers.txt: ${sanitizers:-<none>}"
  else
    sanitizers=""
    echo "No -s and no sanitizers.txt found. Using no sanitizers."
  fi
else
  echo "Sanitizers specified via command-line: ${sanitizers:-<none>}"
fi

# Friendly tweak for macOS AppleClang/leak (IDE/probe compatibility):
# (No-op: CMake enforces final compatibility; keep this section to preserve prior behavior.)
if [[ "$(uname -s)" == "Darwin" ]]; then
  if "$CC_PATH" --version 2>/dev/null | grep -qi "clang"; then
    : # leave as-is; CMake enforces final compatibility
  fi
fi

# ----------------- build dir -----------------
if ! $reuse_build; then
  rm -rf -- "$build_dir"
fi
mkdir -p "$build_dir"

# ----------------- banner -----------------
echo "Configuring with:"
echo "  CC               = $CC_PATH"
echo "  clang-format     = $CLANG_FORMAT_PATH"
echo "  clang-tidy       = $CLANG_TIDY_PATH"
echo "  cppcheck         = $CPPCHECK_PATH"
echo "  sanitizers       = ${sanitizers:-<none>}"
echo "  build dir        = $build_dir"
[[ -n "$generator" ]] && echo "  generator        = $generator"

# ----------------- cmake configure -----------------
cmake_args=(
  -S . -B "$build_dir"
  -DCMAKE_C_COMPILER="$CC_PATH"
  -DCLANG_FORMAT_NAME="$CLANG_FORMAT_PATH"
  -DCLANG_TIDY_NAME="$CLANG_TIDY_PATH"
  -DCPPCHECK_NAME="$CPPCHECK_PATH"
  -DSANITIZER_LIST="$sanitizers"
  -DCMAKE_BUILD_TYPE=Debug
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
)

# Generator if provided
if [[ -n "$generator" ]]; then
  cmake_args+=(-G "$generator")
fi

# Extra -D… after --
if ((${#extra_cmake_args[@]})); then
  cmake_args+=("${extra_cmake_args[@]}")
fi

echo "Running: cmake ${cmake_args[*]}"
cmake "${cmake_args[@]}"

# Only publish a build directory after CMake configured it successfully.
printf '%s\n' "$build_dir" > .last-build-dir
# Runtime artifacts must be safe to load into a consumer built by a different
# compiler.  Sanitized libraries carry a compiler-private runtime and therefore
# are quality-build artifacts, not installable runtime artifacts.
if [[ "${P101_COVERAGE:-0}" != "1" &&
      "${P101_PROFILE:-0}" != "1" &&
      -z "$sanitizers" ]]; then
  printf '%s\n' "$build_dir" > .last-runtime-build-dir
fi

echo "Done. Now run:  ./build.sh"
