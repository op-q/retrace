# v0.1 release checklist

This is the evidence gate for the first process-recorder release. The v0.1
feature slice is implemented, but it is not a supported release until these
checks pass on the exact release candidate.

Record the revision, Linux distribution and kernel, architecture, compiler
versions, CMake version, and commands run. A check that was not run must be
reported as not run rather than assumed to pass.

## Release boundary

v0.1 includes:

- launching one direct target with its arguments and a selected working
  directory;
- process-group creation and `SIGINT`/`SIGTERM` forwarding;
- concurrent stdout and stderr collection;
- direct-target launch, execution, exit, and signal outcomes;
- exclusive user-only v1.0 trace creation;
- streaming trace inspection and structural validation; and
- normal and intentional-crash examples.

v0.1 does not include runtime injection, fault scenarios, time limits, trace
export, descendant lifecycle discovery, environment selection, stream-capture
controls, deterministic replay, authentication, or durable-finalization proof.

## Repository safety

- [ ] Confirm the release is prepared on a topic branch, not `main` or
  `master`.
- [ ] Review `git status --short --branch`, the complete unstaged diff, and the
  complete staged diff.
- [ ] Confirm no `.rtc` trace, crash dump, populated environment file,
  credential, customer output, or identifying production path is tracked.
- [ ] Run `scripts/check-secrets.sh`.
- [ ] Run `git diff --check` and check the release commit with
  `git show --check --oneline`.
- [ ] Confirm CI actions and new dependencies, if any, were intentionally
  reviewed.

## Automated validation

Run the warnings-as-errors build and the complete regular test suite:

```bash
cmake --preset ci
cmake --build --preset ci
ctest --preset ci
```

The current suite contains nine tests. It covers CLI, trace writer, trace
reader, process integration, version and runner smoke tests, the stream fixture,
and the normal and intentional-crash examples.

Run the supported sanitizer configuration:

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

The sanitizer suite currently contains eight tests. It deliberately omits the
intentional-crash example because crashing an instrumented target adds sanitizer
runtime behavior to the recorder test.

Run formatting and static analysis:

```bash
scripts/format.sh --check
cmake --preset ci
mapfile -t sources < <(git ls-files '*.cpp')
clang-tidy --warnings-as-errors='*' -p build/ci "${sources[@]}"
```

- [ ] Warnings-as-errors build passes with a supported GCC version.
- [ ] Warnings-as-errors build passes with a supported Clang version.
- [ ] All nine regular tests pass.
- [ ] All eight supported sanitizer tests pass.
- [ ] Formatting passes.
- [ ] `clang-tidy` passes without unreviewed suppression.

## Manual recorder smoke test

Use a new temporary directory so trace creation does not overwrite an existing
path:

```bash
release_check_dir="$(mktemp -d)"
trace_path="${release_check_dir}/normal.rtc"

./build/ci/bin/retrace run \
  --output "${trace_path}" \
  --working-directory /tmp \
  -- /bin/sh -c 'pwd; printf "normal stdout\n"; printf "normal stderr\n" >&2'

./build/ci/bin/retrace inspect "${trace_path}"
./build/ci/bin/retrace validate "${trace_path}"
```

Inspect the output and trace rather than checking only exit status:

- [ ] The target runs in `/tmp`.
- [ ] stdout and stderr are forwarded to their corresponding caller streams.
- [ ] The trace header contains the resolved working directory and complete
  target argument list.
- [ ] The timeline contains `process.start`, `process.exec`, stream events, and
  `process.exit`.
- [ ] `validate` reports structural validity.
- [ ] Timeline byte fields are quoted and escaped rather than emitted as
  untrusted terminal control text.

Also confirm:

- [ ] A missing executable returns RETRACE launch-failure code `5`.
- [ ] A target exit status of `127` remains distinguishable from launch
  failure.
- [ ] A target terminated by a signal produces the shell-style status and a
  `process.signal` event.
- [ ] Forwarded `SIGINT` and `SIGTERM` reach the target process group and create
  `signal.receive` evidence.
- [ ] An existing output path is not overwritten and the target is not started.
- [ ] A malformed trace is rejected safely.
- [ ] An incomplete final frame preserves and renders preceding complete
  frames, then reports the trace as incomplete.
- [ ] A descendant holding inherited stream descriptors cannot keep collection
  open after the direct target exits.

The automated integration and unit tests exercise these cases; the manual pass
confirms the packaged CLI communicates them correctly.

## Documentation and support claims

- [ ] `README.md` describes the actual release and its most important limits.
- [ ] `cli.md` matches implemented commands, options, diagnostics, and exit
  behavior.
- [ ] `trace-format.md` matches the encoded v1.0 bytes and compatibility tests.
- [ ] `security.md` describes current capture and filesystem behavior.
- [ ] `examples.md` works from a clean build.
- [ ] `roadmap.md` moves future runtime injection and fault work no earlier than
  v0.2 and v0.3.
- [ ] No Docker or Kubernetes support is claimed until container execution has
  its own documented and automated validation.

## Release decision

The release report should state:

- revision and proposed version;
- test, sanitizer, formatting, static-analysis, and secret-scan results;
- manual checks performed;
- skipped checks and why;
- known limitations or open defects; and
- whether the candidate is ready, conditionally ready, or not ready.

A passing checklist shows that the candidate met the documented v0.1 gate. It
does not make trace contents authentic, private, or safe to publish.
