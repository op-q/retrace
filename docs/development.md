# Development design

## Repository layout

The current scaffold is intentionally smaller than the final architecture:

```text
include/retrace/       public C/C++ interfaces
src/                   C++ application and core implementation
tests/                 unit and integration tests plus deterministic fixtures
cmake/                 reusable build policies and generated-header templates
docs/                  product and engineering documentation
scripts/               local formatting, testing, and safety checks
.githooks/             versioned opt-in Git hooks
.github/               CI and collaboration templates
```

`src/` is split into CLI, process, and trace components; a fault component will
be added with that milestone. `runtime/` will contain the C shared library only
when milestone v0.2 begins. `examples/`, `scenarios/`, and integration fixtures
should be added with the behavior they demonstrate rather than as empty
directories.

## Technology choices

- GCC and Clang are the supported compilers; CI checks both.
- CMake and Ninja provide the initial build workflow.
- Production code uses C++20; the injected runtime uses C17.
- The initial CLI parser and test harness are deliberately dependency-free.
- TOML is the planned scenario format.
- Trace framing and parsing are implemented directly while the event model is
  small.
- clang-format, clang-tidy, compiler warnings, and sanitizers provide overlapping
  quality checks.

A third-party CLI parser, test framework, or serialization library may be added
when the project can state the concrete maintenance benefit. Dependency count is
not a goal by itself, but every dependency expands review and supply-chain work.

## Error model

RETRACE must distinguish:

- its own usage or internal failure;
- failure to launch the target;
- a target's non-zero exit, signal, or crash;
- unavailable runtime instrumentation or an unsupported target;
- malformed scenarios or traces;
- event-channel and trace-write failures; and
- partial traces that remain usable.

Errors name what failed, which component detected it, whether the target ran,
whether a partial trace exists, and a useful next action. Internal diagnostics
remain separate from captured target output.

## Testing policy

The current tests cover CLI behavior, file-descriptor and pipe ownership,
process launch and lifecycle events, separate stdout/stderr routing, concurrent
collection of output larger than a pipe's capacity, trace metadata and framing,
exclusive user-only file creation, CLI recording, and recovery of complete
frames before a truncated tail. Reader tests use independently encoded bytes to
exercise header and frame bounds, unsupported versions, known payload schemas,
nondecreasing timestamps, unknown event identifiers, and every truncation point
across representative frames. CLI tests cover inspection, structural
validation, escaped output, failed output streams, and their usage, I/O, and
trace-format exit codes. Process tests also verify that a descendant retaining
inherited stream descriptors cannot hold the direct target's capture open.

As features land, unit tests will cover duration, signal, and rule parsing; path
matching; encoding and decoding; format bounds; rendering; and corrupt payloads.
Integration fixtures will grow to cover crashes, signal termination, child
creation, stream interleaving, file and socket outcomes, delays, and forced
partial traces. Runtime tests will cover symbol resolution, recursion guards,
`errno`, threads, unavailable channels, payload bounds, long paths, partial
writes, and activation counts.

Reader and writer compatibility tests protect the v1.0 byte order, metadata
layout, frame layout, bounds, truncation boundary, corrupt-input handling, and
unknown future events. Timeline tests protect the current terminal rendering.
JSON output remains planned.

AddressSanitizer and UndefinedBehaviorSanitizer run together in CI. Static
analysis and formatting also run in CI. ThreadSanitizer will be introduced where
its Linux and runtime constraints make results dependable. Findings are fixed or
narrowly justified rather than suppressed globally.

## Performance and self-observation

Measure target overhead, event and trace throughput, memory use, intercepted-call
cost, stream-capture cost, and multi-threaded behavior. Every published number
names its hardware, compiler, build mode, enabled events, event count, destination,
and compression state.

Verbose diagnostics may include pipe creation, runtime path, target PID, trace
destination, handshake state, event and drop counts, and finalization state.
They must not contaminate target stdout or stderr.

## Documentation definition of done

Before a release, document installation, supported platforms and target types,
commands, trace format, rule syntax, `LD_PRELOAD` limits, security considerations,
performance overhead, known issues, architecture, examples, and both guarantees
and non-guarantees.

The README remains a quick entry point. Detailed decisions belong in focused
documents, and behavior-changing pull requests update them with the code.
