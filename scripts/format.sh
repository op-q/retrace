#!/usr/bin/env bash
set -euo pipefail

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "${script_directory}/.." && pwd)
cd "${project_root}"

if ! command -v clang-format >/dev/null 2>&1; then
  echo "error: clang-format is required" >&2
  exit 1
fi

format_arguments=(-i)
if (($# > 0)); then
  if [[ $1 != "--check" || $# -ne 1 ]]; then
    echo "usage: scripts/format.sh [--check]" >&2
    exit 2
  fi
  format_arguments=(--dry-run --Werror)
fi

mapfile -d '' source_files < <(
  git ls-files --cached --others --exclude-standard -z -- \
    '*.c' '*.h' '*.cpp' '*.hpp'
)

if ((${#source_files[@]} > 0)); then
  clang-format "${format_arguments[@]}" -- "${source_files[@]}"
fi
