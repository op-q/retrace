# Product goals and scope

## Thesis

Production failures often depend on timing, operating-system behavior, partial
I/O, unavailable files, slow dependencies, signals, crashes, or unusual event
sequences. Application logs describe what a program believed it was doing; they
do not always show what happened at its process boundary.

RETRACE should create a compact, trustworthy timeline of observable behavior and
then recreate selected failure conditions under controlled, recorded rules.

## Goals

RETRACE should:

- launch and supervise Linux processes without requiring root;
- record process start/exit, signals, duration, stdout, and stderr;
- later observe selected file and socket operations through an injected runtime;
- persist a versioned, crash-tolerant, streaming trace;
- render a readable terminal timeline and export reviewable data;
- inject a deliberately small set of deterministic, bounded faults;
- record exactly what was observed and every action it injected;
- remain testable with small deterministic example programs; and
- keep its architecture narrow and understandable.

Engineering the tool provides practical work with `fork`, `execve`, `waitpid`,
process groups, signals, descriptors, pipes, Unix-domain sockets, dynamic
loading, C/C++ interoperability, RAII, framing, concurrency, and failure
handling.

## Intended users

- Backend engineers reproducing process-level failures
- Systems programmers studying Linux runtime behavior
- Developers testing crash recovery and retry behavior
- Library authors testing partial or failed I/O
- Students learning process supervision, IPC, and native failure handling
- Engineers whose services depend on files, sockets, signals, and child processes

## Non-goals for early versions

RETRACE does not initially provide:

- deterministic or instruction-level replay;
- a debugger, breakpoints, source stepping, profiling, or complete syscall tracing;
- kernel modules, eBPF, or a `ptrace`-based debugger;
- distributed tracing or multi-host coordination;
- a GUI, Windows, or macOS support;
- container orchestration, arbitrary process injection, or root-level monitoring;
- isolation equivalent to a container or sandbox; or
- full file contents or network payload capture.

These are explicit boundaries, not hidden deficiencies. Research may revisit
some of them only after the core recorder is finished.

## Core use cases

The implemented workflow covers the first two steps and the inspect/validate
parts of step three. Filtering and export remain planned:

1. Run a command and preserve its arguments.
2. Capture lifecycle metadata and configured streams.
3. Inspect, filter, validate, or export its trace.
4. Add a time limit, signal, or reusable fault scenario.
5. Compare normal and failing behavior without calling that deterministic replay.

Inspection renders a readable terminal timeline and validation checks the v1.0
binary structure. Neither operation authenticates trace contents or proves that
a frame-aligned recording was finalized; v1.0 has no footer or checksum.

See [cli.md](cli.md) for the command shapes and [fault-rules.md](fault-rules.md)
for scenarios.

## Demonstration target

Consider a C program that assumes opening a cache always succeeds:

```c
int fd = open("/tmp/cache/state.bin", O_RDONLY);
read(fd, buffer, sizeof(buffer));
```

A future RETRACE scenario deliberately returns `EACCES` from that `open`. The
trace records the rule match, injected failure, failed operation, and any
resulting crash. The corrected program checks ownership of the returned file
descriptor before using it:

```c
int fd = open("/tmp/cache/state.bin", O_RDONLY);
if (fd < 0) {
    fprintf(stderr, "cache unavailable: %s\n", strerror(errno));
    return EXIT_FAILURE;
}
```

This is the product in miniature: create a controlled failure, prove where it
occurred, observe the program's response, and verify a correction.

## Guarantees and language

Documentation and output must not claim deterministic replay, full syscall
visibility, complete child-process coverage, crash-proof tracing, or successful
fault injection until the corresponding behavior was observed and tested. Use
precise phrases such as “recreate selected conditions,” “rerun with a recorded
scenario,” and “compare traces.”
