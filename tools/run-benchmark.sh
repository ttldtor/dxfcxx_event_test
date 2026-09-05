#!/usr/bin/env bash
set -euo pipefail

binary_directory=""
arguments=("$@")

for ((index = 0; index < ${#arguments[@]}; ++index)); do
    if [[ "${arguments[$index]}" == "--binary-directory" && $((index + 1)) -lt ${#arguments[@]} ]]; then
        binary_directory=${arguments[$((index + 1))]}
        break
    fi
done

if [[ -z "$binary_directory" ]]; then
    echo "--binary-directory is required" >&2
    exit 2
fi

runner="$binary_directory/latency_runner"

if [[ ! -x "$runner" ]]; then
    echo "Missing benchmark runner: $runner" >&2
    exit 2
fi

exec "$runner" "$@"
