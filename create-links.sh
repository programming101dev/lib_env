#!/usr/bin/env bash
set -euo pipefail

# Function to display usage information
usage()
{
    echo "Usage: $0 <source_directory>"
    exit 1
}

# --help / -h -> usage, exit 0 (P101 uniform CLI help)
case " $* " in *" --help "*|*" -h "*) ( usage ) || true; exit 0 ;; esac

# Check if exactly one argument is provided
if [ "$#" -ne 1 ]; then
    usage
fi

source_dir="$(CDPATH='' cd -- "$1" 2>/dev/null && pwd -P)" || {
    echo "Error: Source directory '$1' does not exist." >&2
    exit 1
}
dest_dir="$(pwd)"

ensure_link() {
    local source_file="$1" dest_file="$2" label="$3"
    [ -e "$source_file" ] || {
        echo "Error: required source is missing or dangling: $source_file" >&2
        return 1
    }
    if [ -L "$dest_file" ]; then
        ln -sfn -- "$source_file" "$dest_file"
        echo "Updated $label link in $dest_dir"
    elif [ -e "$dest_file" ]; then
        echo "Error: refusing to replace non-symlink path: $dest_file" >&2
        return 1
    else
        ln -s -- "$source_file" "$dest_file"
        echo "Linked $label to $dest_dir"
    fi
}

# List of files to link
files_to_link=("sanitizers.txt" "supported_c_compilers.txt")

for file in "${files_to_link[@]}"; do
    source_file="$source_dir/$file"
    dest_file="$dest_dir/$file"

    ensure_link "$source_file" "$dest_file" "$file"
done

# Special handling for .flags. It may itself be a symlink to a shared,
# expensive compiler-flag cache, but it must be provided by the explicit source
# directory rather than discovered from a parent layout.
flags_source="$source_dir/.flags"

flags_dest="$dest_dir/.flags"

ensure_link "$flags_source" "$flags_dest" ".flags"
