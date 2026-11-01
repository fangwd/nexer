#!/usr/bin/env bash

set -euo pipefail

EXTENSIONS=("h" "hpp" "cc" "cpp")

usage() {
    echo "Usage: $0 [file]"
    echo ""
    echo "  $0             Format all .h, .hpp, .cc, .cpp files recursively"
    echo "  $0 <file>      Format a single file"
    exit 1
}

format_file() {
    echo "Formatting: $1"
    clang-format -i "$1"
}

if [[ $# -gt 1 ]]; then
    usage
elif [[ $# -eq 1 ]]; then
    if [[ ! -f "$1" ]]; then
        echo "Error: '$1' is not a file."
        exit 1
    fi
    format_file "$1"
else
    # Build the -name pattern for find
    find_args=()
    for ext in "${EXTENSIONS[@]}"; do
        if [[ ${#find_args[@]} -gt 0 ]]; then
            find_args+=("-o")
        fi
        find_args+=("-name" "*.${ext}")
    done

    while IFS= read -r -d '' file; do
        format_file "$file"
    done < <(find core/ http/ runtime/ apps/ examples/ tests/ \( "${find_args[@]}" \) -print0 2>/dev/null)

    echo "Done."
fi
