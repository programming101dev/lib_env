#!/usr/bin/env bash

skip_cache_update=false

# Parse command-line options
while getopts ":s" opt; do
  case $opt in
    s)
      skip_cache_update=true
      ;;
    \?)
      echo "Invalid option: -$OPTARG" >&2
      exit 1
      ;;
  esac
done

if cmake --install build; then
    echo "Installation successful."

    # Retrieve the owner of the 'build' directory
    if [ "$(uname -s)" = "Darwin" ]; then
        build_owner=$(stat -f "%Su" build)
    else
        build_owner=$(ls -ld "build" | awk '{print $3}')  # Linux
    fi

    # Change the ownership of install_manifest.txt to match 'build' directory owner
    chown "$build_owner" build/install_manifest.txt

    if [ "$skip_cache_update" = false ]; then
        # Check if the command 'ldconfig' exists on the system
        if command -v ldconfig >/dev/null; then
            # 'ldconfig' exists, run it with sudo
            sudo ldconfig
        elif command -v update_dyld_shared_cache >/dev/null; then
            # 'ldconfig' doesn't exist, but 'update_dyld_shared_cache' does, run it with sudo
            sudo update_dyld_shared_cache -force
        fi
    fi
else
    echo "Installation failed or not supported."
    # Handle the failure (e.g., print a message or attempt an alternative installation method)
fi
