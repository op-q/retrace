#!/usr/bin/env bash
# Record a target and keep both artifacts: the binary trace and its rendered
# timeline. Every run gets a fresh timestamped pair under logs/, so the
# exclusive-create trace path never collides with an earlier recording.
set -euo pipefail

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "${script_directory}/.." && pwd)
cd "${project_root}"

retrace_binary="${project_root}/build/dev/bin/retrace"
if [[ ! -x ${retrace_binary} ]]; then
  echo "error: ${retrace_binary} is missing; run scripts/test.sh first" >&2
  exit 1
fi

# Name the pair after the target program so a logs/ listing stays readable.
separator_seen=0
target_name=""
for argument in "$@"; do
  if ((separator_seen)); then
    target_name=$(basename -- "${argument}")
    break
  fi
  if [[ ${argument} == "--" ]]; then
    separator_seen=1
  fi
done

if ((!separator_seen)) || [[ -z ${target_name} ]]; then
  echo "usage: scripts/record.sh [RUN_OPTIONS] -- COMMAND [ARGS...]" >&2
  exit 2
fi

target_name=${target_name//[^A-Za-z0-9._-]/_}

# Each run owns one directory holding its trace and rendered timeline.
run_directory="${project_root}/logs/$(date +%Y%m%d-%H%M%S)-${target_name}"
# Two runs within the same second must not share a directory.
attempt=0
while ! mkdir -p "${run_directory}" 2>/dev/null || [[ -e ${run_directory}/trace.rtc ]]; do
  attempt=$((attempt + 1))
  if ((attempt > 100)); then
    echo "error: could not create a run directory under ${project_root}/logs" >&2
    exit 1
  fi
  run_directory="${run_directory%-*}-${attempt}"
done

trace_path="${run_directory}/trace.rtc"
timeline_path="${run_directory}/timeline.txt"

# Render on every exit path. A trace abandoned by an interrupted or wedged run
# still holds complete frames, and those are exactly the ones worth reading.
render_timeline() {
  if [[ -e ${trace_path} ]]; then
    if ! "${retrace_binary}" inspect "${trace_path}" >"${timeline_path}"; then
      echo "warning: the trace could not be fully rendered" >&2
    fi
  fi
  echo "logs: ${run_directory}" >&2
}
trap render_timeline EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

status=0
"${retrace_binary}" run --output "${trace_path}" "$@" || status=$?
exit "${status}"
