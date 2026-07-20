# Architecture

## Product boundary

Today, RETRACE launches one command, waits for it, and returns its exit status.
The target inherits the terminal's stdout and stderr. Trace writing, stream
capture, process groups, signal forwarding, and fault injection are not yet
implemented.

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

The current CLI parses `help`, `version`, and `run`, validates their arguments,
and returns documented exit codes. Scenario loading and output selection will
be added with their corresponding features. The public interface should remain
stable even while internals evolve.

### Supervisor

The current C++ process layer owns command launch and waiting. It distinguishes
a launch failure from a target that exits with status 127 and preserves normal
exit and signal status. The planned supervisor will also own pipes, process
groups, signal forwarding, stream collection, runtime-event collection, and
trace finalization. It must state whether a usable partial trace exists.

The planned launch sequence is:

1. Parse the command and scenario.
2. Create the trace and stdout, stderr, and event pipes.
3. Call `fork()`.
4. In the child, create the target process group, redirect descriptors,
   configure the runtime environment, and call `execve()`.
5. In the parent, close unused pipe ends, record `process.start`, collect
   events, forward signals, and wait with `waitpid()`.
6. Flush all complete frames and finalize the trace.

Child-process support will grow in phases: supervise the original target first,
then report `fork`/`exec`, then track descendants in the process group. Linux
subreaper behavior may be evaluated later.

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

This component is planned, not implemented. Trace persistence will remain
independent from live process supervision. Writers will append frames without
holding the entire trace in memory. Readers will treat every file as untrusted
and recover complete preceding frames when the end is truncated.

### Fault engine

This component is planned, not implemented. The fault engine will parse,
compile, and match explicit rules. Every injected action must emit trace
evidence. Counts, seeds, and activation limits will be stored so a scenario is
explainable and reproducible within its documented limits.

## C and C++ responsibilities

C++20 is used for the current CLI, process execution, tests, and file-descriptor
ownership. It will also implement supervision, trace I/O, scenario parsing,
rendering, and concurrency. C++ RAII types will own resources such as pipes,
processes, threads, mappings, trace files, temporary directories, and restored
signal masks as those features arrive.

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
