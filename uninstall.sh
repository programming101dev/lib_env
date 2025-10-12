#!/usr/bin/env bash
# uninstall.sh — remove installed headers/libs/pkgconfig/cmake for a lib
# Works on macOS, Linux, *BSD. Uses sudo only when required.
set -euo pipefail

# -------- defaults --------
NAME="p101_env"
ASSUME_YES=false
DRY_RUN=false
VERBOSE=false

# Capitalized variant for some CMake package dir names (portable for bash 3.2)
CAP_NAME="$(printf '%s' "$NAME" | awk '{print toupper(substr($0,1,1)) substr($0,2)}')"

# Default prefixes per platform (searched in order)
OS="$(uname -s)"
if [[ "$OS" == "Darwin" ]]; then
  PREFIX_DEFAULTS=("/usr/local" "/opt/homebrew")
else
  PREFIX_DEFAULTS=("/usr/local" "/usr")
fi

# user-defined prefixes (may be empty; must be declared!)
declare -a PREFIXES=()

usage() {
  cat <<USAGE
Usage: $0 [-n] [-y] [-v] [-N <name>] [-p <prefix> ...]
  -n            Dry run (show what would be removed, but don't delete)
  -y            Assume 'yes' to prompts (non-interactive)
  -v            Verbose
  -N <name>     Override library/package name (default: ${NAME})
  -p <prefix>   Add install prefix to search (repeatable). Searched BEFORE defaults.
Examples:
  $0
  $0 -n -v
  $0 -N p101_env -p /custom/prefix
USAGE
  exit 1
}

# -------- opts --------
while getopts ":nyvN:p:h" opt; do
  case "$opt" in
    n) DRY_RUN=true ;;
    y) ASSUME_YES=true ;;
    v) VERBOSE=true ;;
    N) NAME="$OPTARG"; CAP_NAME="$(printf '%s' "$NAME" | awk '{print toupper(substr($0,1,1)) substr($0,2)}')" ;;
    p) PREFIXES+=("$OPTARG") ;;
    h|*) usage ;;
  esac
done

# -------- helpers --------
log()   { $VERBOSE && printf '[info] %s\n' "$*"; }
say()   { printf '%s\n' "$*"; }
err()   { printf '[error] %s\n' "$*" >&2; }

confirm() {
  $ASSUME_YES && return 0
  printf '%s [y/N] ' "$*"
  read -r ans || ans=""
  [[ "$ans" == "y" || "$ans" == "Y" ]]
}

needs_sudo() {
  # returns 0 if we should use sudo for the given path (permission check)
  local p="$1"
  local parent
  parent="$(dirname "$p")"
  [[ -w "$p" || -w "$parent" ]] && return 1 || return 0
}

do_rm() {
  # do_rm [-r] path
  local recursive=""
  if [[ "${1:-}" == "-r" ]]; then recursive="-r"; shift; fi
  local path="$1"

  if $DRY_RUN; then
    say "DRY-RUN: rm $recursive -f -- $path"
    return 0
  fi

  if needs_sudo "$path"; then
    log "Using sudo to remove: $path"
    sudo rm $recursive -f -- "$path" || return $?
  else
    rm $recursive -f -- "$path" || return $?
  fi
}

do_rmdir() {
  # remove empty directory if present (best effort)
  local d="$1"
  [[ -d "$d" ]] || return 0
  if $DRY_RUN; then
    say "DRY-RUN: rmdir --ignore-fail-on-non-empty -- $d"
    return 0
  fi
  if needs_sudo "$d"; then
    sudo rmdir "$d" 2>/dev/null || true
  else
    rmdir "$d" 2>/dev/null || true
  fi
}

dedupe_append() {
  # dedupe_append arr_name items...
  local arr="$1"; shift
  local x
  for x in "$@"; do
    [[ -z "${x// /}" ]] && continue
    if ! eval 'for _y in "${'"$arr"'[@]:-}"; do [[ "$_y" == "'"$x"'" ]] && _hit=1; done; [[ -n "${_hit:-}" ]] && unset _hit || :'; then :; fi
    if ! eval 'for _y in "${'"$arr"'[@]:-}"; do [[ "$_y" == "'"$x"'" ]] && return 0; done'; then
      eval "$arr+=(\"\$x\")"
    fi
  done
}

# Compose search prefixes: user-provided first, then defaults (deduped)
declare -a SEARCH_PREFIXES=()
dedupe_append SEARCH_PREFIXES "${PREFIXES[@]:-}"
dedupe_append SEARCH_PREFIXES "${PREFIX_DEFAULTS[@]}"

if [[ ${#SEARCH_PREFIXES[@]} -eq 0 ]]; then
  err "No prefixes to search."
  exit 2
fi

say "Uninstalling '${NAME}' from prefixes: ${SEARCH_PREFIXES[*]}"

# Build the list of paths to remove
declare -a TARGETS=()

for P in "${SEARCH_PREFIXES[@]}"; do
  # Headers dir
  TARGETS+=("$P/include/${NAME}")

  # Libraries (common dirs)
  for L in "$P/lib" "$P/lib64" "$P/lib/arm64" "$P/lib/aarch64-linux-gnu" "$P/lib/x86_64-linux-gnu"; do
    TARGETS+=("$L/lib${NAME}.a")
    TARGETS+=("$L/lib${NAME}.so")
    TARGETS+=("$L/lib${NAME}.so.0")
    TARGETS+=("$L/lib${NAME}.so.*")            # versioned symlinks
    TARGETS+=("$L/lib${NAME}.dylib")
    TARGETS+=("$L/lib${NAME}.*.dylib")         # versioned on macOS
    TARGETS+=("$L/lib${NAME}.la")              # libtool files (if any)

    # CMake config locations
    TARGETS+=("$L/cmake/${NAME}")
    TARGETS+=("$L/cmake/${CAP_NAME}")          # capitalized variant (portable)

    # pkg-config
    TARGETS+=("$L/pkgconfig/${NAME}.pc")
    TARGETS+=("$L/pkgconfig/${NAME}-*.pc")
  done

  # share variants
  TARGETS+=("$P/share/${NAME}")
  TARGETS+=("$P/share/${NAME}/cmake")
  TARGETS+=("$P/share/cmake/${NAME}")
  TARGETS+=("$P/share/cmake/${CAP_NAME}")
  TARGETS+=("$P/share/pkgconfig/${NAME}.pc")
  TARGETS+=("$P/share/pkgconfig/${NAME}-*.pc")
done

# Filter to those that actually exist (expand globs safely)
declare -a EXISTING=()
shopt -s nullglob
for pat in "${TARGETS[@]}"; do
  for hit in $pat; do
    [[ -e "$hit" || -L "$hit" ]] && EXISTING+=("$hit")
  done
done
shopt -u nullglob

if [[ ${#EXISTING[@]} -eq 0 ]]; then
  say "Nothing found to remove for '${NAME}'."
  exit 0
fi

say "The following paths will be removed (${#EXISTING[@]}):"
for p in "${EXISTING[@]}"; do
  echo "  $p"
endone 2>/dev/null || true  # in case of very long lists; harmless

if ! confirm "Proceed with removal?"; then
  say "Aborted."
  exit 1
fi

# Remove files/dirs
for p in "${EXISTING[@]}"; do
  if [[ -d "$p" && ! -L "$p" ]]; then
    log "Removing directory: $p"
    do_rm -r "$p" || err "Failed to remove $p"
  else
    log "Removing file: $p"
    do_rm "$p" || err "Failed to remove $p"
  fi
done

# Attempt to clean up now-empty dirs for neatness (best-effort)
for P in "${SEARCH_PREFIXES[@]}"; do
  do_rmdir "$P/share/pkgconfig"
  do_rmdir "$P/lib/pkgconfig"
  do_rmdir "$P/lib/cmake/${NAME}" || true
  do_rmdir "$P/lib/cmake/${CAP_NAME}" || true
  do_rmdir "$P/lib/cmake" || true
  do_rmdir "$P/share/cmake/${NAME}" || true
  do_rmdir "$P/share/cmake/${CAP_NAME}" || true
  do_rmdir "$P/share/cmake" || true
done

say "Uninstall complete for '${NAME}'."

# Refresh linker caches (if not dry-run)
if ! $DRY_RUN; then
  if command -v ldconfig >/dev/null 2>&1; then
    log "Running ldconfig"
    if needs_sudo "/sbin/ldconfig"; then sudo ldconfig; else ldconfig; fi
  elif command -v update_dyld_shared_cache >/dev/null 2>&1; then
    log "Running update_dyld_shared_cache (macOS)"
    if needs_sudo "/usr/sbin/update_dyld_shared_cache"; then
      sudo update_dyld_shared_cache -force || true
    else
      update_dyld_shared_cache -force || true
    fi
  fi
fi
