#!/usr/bin/env bash
# Developer fast path: configure, build, and run the default debug preset from
# any current directory. CI uses the stricter `ci` and `asan` presets directly.
set -euo pipefail

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "${script_directory}/.." && pwd)
cd "${project_root}"

cmake --preset dev
cmake --build --preset dev
ctest --preset dev
