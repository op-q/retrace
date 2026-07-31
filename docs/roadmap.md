# Roadmap

Each milestone must produce a narrow, usable result. Research items stay out of
the main implementation until current acceptance criteria are met.

## Scaffold — complete

- CMake project with C17 and C++20 enabled
- `retrace version` and help
- dependency-free CLI tests
- compiler warnings, optional sanitizers, formatting, and CI
- documentation and repository safety guardrails

## v0.1 implementation — process recorder

The feature slice is implemented. This means the behavior below exists and has
focused automated coverage; it does not yet mean that a supported v0.1 release
has been published.

- [x] Add move-only RAII file-descriptor and pipe owners.
- [x] Launch a command with `fork()` and `execvpe()`.
- [x] Preserve target arguments.
- [x] Distinguish an `exec` failure from target exit code 127.
- [x] Preserve ordinary exit codes and terminating signals.
- [x] Capture stdout and stderr with pipes.
- [x] Create a versioned trace and lifecycle events.
- [x] Inspect and validate traces.
- [x] Create a process group and forward signals.
- [x] Select and record a target working directory.
- [x] Document normal and intentional-crash examples.

## v0.1 release boundary

The first release is the Linux process recorder described below. Runtime
injection, fault scenarios, container support, and broader process-tree
visibility remain outside this boundary. Before calling a revision v0.1, run
and record every applicable item in the
[v0.1 release checklist](release-checklist.md).

Commands:

```bash
retrace run -- COMMAND [ARGS...]
retrace inspect TRACE
retrace validate TRACE
```

Work sequence:

1. Add RAII file-descriptor and pipe types.
2. Launch a command with `fork()` and `execvpe()` using an explicitly rebuilt
   environment when supervisor configuration requires it.
3. Capture its exit code or terminating signal.
4. Capture stdout and stderr while preserving order as accurately as practical.
5. Write a versioned, framed, crash-tolerant trace.
6. Inspect and validate the trace.
7. Forward `SIGINT`/`SIGTERM` to the target process group.

Acceptance criteria:

- target arguments and a selected working directory are preserved;
- stdout, stderr, normal exit, non-zero exit, and signal termination are stored;
- interruption is forwarded and recorded;
- complete frames survive an incomplete final write;
- inspection produces a readable timeline;
- validation rejects malformed inputs safely;
- unit, integration, and supported sanitizer tests pass; and
- normal and crashing example programs are documented.

Implementation acceptance is complete. Release readiness still requires a
fresh candidate-wide safety, toolchain, documentation, and manual behavior pass.

## v0.2 — injected C runtime

Add `libretrace_runtime.so`, a runtime handshake, event transport, `open`/
`openat`, `close`, `connect`, and selected read/write metadata.

Acceptance criteria include recursion prevention, `errno` preservation,
multi-threaded fixtures, bounded payloads, and graceful behavior when the event
channel is absent. Unsupported static/setuid targets are reported rather than
silently misrepresented.

Initial progress:

- [x] Build a C17 `libretrace_runtime.so`.
- [x] Define and emit a versioned, bounded handshake frame.
- [x] Keep absent, invalid, and closed event channels nonfatal.
- [x] Create and validate the runtime channel in the supervisor.
- [x] Load the runtime into supported dynamic targets.
- [ ] Interpose and record the selected libc operations.

## v0.3 — fault injection

Add failed matching file opens, delayed matching connections, scheduled signals
or termination, scenario files, rule counts, activation limits, and deterministic
seeds. Every action creates trace evidence, and tests distinguish observed from
injected failures.

## v0.4 — reliability scenarios

Publish polished normal/failing/corrected examples for interrupted writes, slow
dependencies, incomplete files after termination, mishandled `SIGTERM`, and
retry behavior after a refused connection.

## v0.5 — trace comparison

A possible `retrace compare successful.rtc failing.rtc` command identifies a
divergence point, changed results, timing differences, missing/extra operations,
and different exit conditions.

## Future research

Research may explore `ptrace`, seccomp user notifications, eBPF, `io_uring`,
namespace isolation, Chrome trace export, symbolization, stack traces, source
mapping, process-tree visualization, and deterministic replay. None of these
may delay a finished process recorder and the first two useful fault classes.

## Test matrix as features land

Unit tests will cover parsing, matching, encoding, bounds, formatting, status
rendering, and corrupt inputs. Integration fixtures will cover clean and
non-zero exits, signals and crashes, child processes, stream interleaving, file
and socket outcomes, delayed connections, and partial traces. Runtime tests will
cover symbol resolution, recursion, `errno`, concurrency, unavailable channels,
oversized payloads, long paths, partial writes, and rule counts.
