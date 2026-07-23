# Architecture

## Product boundary

Today, RETRACE launches one command, collects its stdout and stderr concurrently,
forwards both streams, selects a target working directory, and can write command
metadata and observed lifecycle and stream events to a versioned trace. Each
target leads a process group; RETRACE synchronously receives `SIGINT` and
`SIGTERM`, forwards them to that group, and records successful forwarding. It can
also validate a trace's v1.0 structure and render its complete events as a
terminal timeline. Trace export and fault injection are not yet implemented.

The target architecture will supervise the command, record selected runtime
events, and write them to a trace. Later it will reproduce explicitly configured
failures. RETRACE is not a security sandbox, debugger, profiler, full syscall
tracer, or deterministic execution-replay system.

The intended responsibility boundaries are:

```text
CLI
  └── Supervisor
        ├── target process and descendants
        ├── stdout/stderr collection
        ├── injected C runtime event channel (v0.2+)
        └── trace writer

Trace file
  └── reader / validator / renderer / exporter
```

## Components

### CLI

The current CLI parses `help`, `version`, `run`, `inspect`, and `validate`,
including an explicit `run --output TRACE` recording path. It validates command
arguments and returns documented exit codes. Scenario loading, export, filters,
and further output selection will be added with their corresponding features.
The public interface should remain stable even while internals evolve.

### Supervisor

The current C++ process layer owns command launch, the target process group,
close-on-exec pipes, synchronous signal forwarding, concurrent stream
collection, typed lifecycle events, and waiting. It distinguishes a launch
failure from a target that exits with status 127 and preserves normal exit and
signal status. A bounded buffer feeds events to a callback instead of
accumulating all target output in memory. The planned supervisor will also own
runtime-event collection. It must state whether a usable partial trace exists.

Child status is checked while the stream pipes are polled. Once the direct child
is reaped, the collector snapshots and drains only bytes already queued in each
pipe, then closes its read ends. This preserves buffered direct-child output
without allowing a descendant that inherited a write end to extend collection
forever. Descendants share the target process group for signal delivery, but
RETRACE still waits for and reports only the direct target.

The current launch sequence is:

1. Parse the command and validate the selected working directory.
2. Create the trace, stdout/stderr pipes, launch-status pipe, and signal source.
3. Call `fork()`.
4. In the child, create the target process group, restore the inherited signal
   mask, change directory, redirect descriptors, and call `execvp()`.
5. In the parent, close unused pipe ends, record `process.start`, poll streams
   and the signal source, forward signals with `kill(-pgid, signal)`, and check
   completion with `waitpid()`.
6. Finish all complete frame writes and close the trace. Version 1.0 has no
   footer or durable-finalization record.

Child-process support will grow in phases. The original target is supervised and
descendants receive group-directed signals, but descendant discovery and event
tracking remain later work. Linux subreaper behavior may be evaluated later.

### Injected runtime

Planned for v0.2, a deliberately small C shared library will be loaded into
dynamically linked targets through `LD_PRELOAD`. It will:

- interpose selected libc calls;
- resolve the real symbol with `dlsym(RTLD_NEXT, ...)`;
- match bounded fault rules;
- emit compact events through a framed C-compatible protocol;
- use thread-local recursion guards;
- preserve `errno`; and
- normally let the target continue if tracing becomes unavailable.

It cannot reliably instrument static binaries, setuid binaries, direct syscalls,
or every language runtime. RETRACE must report these limits honestly.

### Trace reader and writer

The v1.0 writer and reader are implemented independently from live process
supervision. The writer creates a new user-only file, appends bounded frames
without holding the entire trace in memory, and stops after a write failure so
an incomplete frame remains the final frame.

The CLI explicitly finishes an open writer and checks the final `close(2)`
result before reporting recording success. It does not claim `fsync(2)`-level
durability, and writer calls must remain serialized so frame writes cannot
interleave.

The move-only reader owns its descriptor and decoded header. It validates the
fixed prefix and supported version, applies size limits before allocating,
decodes metadata through a checked cursor, and reads one owned event at a time.
Known event payloads and nondecreasing timestamps are checked; otherwise valid
unknown event identifiers remain readable for forward extension. End of input,
an incomplete final frame, malformed input, and operating-system read errors are
distinct outcomes.

The validator consumes this stream without rendering it. The inspector renders
the same complete prefix with bounded, escaped byte previews. A clean end is
reported as structurally valid, but v1.0 has no footer or checksum: it cannot
prove that the writer finalized the run, that a frame-aligned suffix was not
lost, or that the bytes are authentic.

### Fault engine

This component is planned, not implemented. The fault engine will parse,
compile, and match explicit rules. Every injected action must emit trace
evidence. Counts, seeds, and activation limits will be stored so a scenario is
explainable and reproducible within its documented limits.

## C and C++ responsibilities

C++20 is used for the current CLI, process execution, trace I/O and rendering,
tests, and file-descriptor ownership. It will also implement broader
supervision, scenario parsing, and concurrency. C++ RAII types own current pipes
and trace descriptors and will own processes, threads, mappings, temporary
directories, and restored signal masks as those features arrive.

C17 is used for the injected runtime because it crosses a C ABI and runs inside
someone else's process. Keeping that code small avoids pulling the C++ runtime,
exceptions, or complex object initialization into the target.

The two sides exchange bytes defined by a C-compatible protocol header. They do
not share C++ objects or assume that an in-memory C struct is automatically a
portable on-disk format.

## Design principles

- Truth over spectacle: report only visibility and injection that occurred.
- Small useful versions: finish process recording before runtime interposition.
- Failure is part of the product: test crashes, full disks, closed pipes,
  malformed messages, unsupported binaries, races, partial writes, and slow
  consumers.
- Linux first: isolate platform-specific code without weakening the Linux design.
- Minimal dependencies: introduce a library only when its value exceeds its
  review, build, and supply-chain costs.

## Key risks

| Risk | Initial mitigation |
| --- | --- |
| Scope expands into several different tools | Enforce non-goals and milestone acceptance criteria |
| Runtime hooks recurse | Thread-local guards, direct transport, minimal hook dependencies |
| Runtime destabilizes the target | Preserve `errno`, avoid allocation-heavy paths, provide `--no-runtime` |
| A crash corrupts the trace | Append-only length-prefixed frames and tolerant readers |
| Event volume creates excessive overhead | Metadata-only defaults, filtering, bounds, and dropped-event counters |
| Users infer deterministic replay | Say “recreate selected conditions” and “compare traces” |

Performance claims must name the hardware, compiler, build mode, event types,
event count, trace destination, and compression setting used for measurement.
