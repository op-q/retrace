#!/usr/bin/env bash
set -euo pipefail

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "${script_directory}/.." && pwd)
cd "${project_root}"

cmake --preset dev
cmake --build --preset dev
ctest --preset dev
