# RETRACE

[![CI](https://github.com/op-q/retrace/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/op-q/retrace/actions/workflows/ci.yml)

RETRACE is a native Linux process recorder and fault-injection tool. Its goal
is to show important process-boundary behavior and then recreate selected
failure conditions deliberately.

> A program failed under unusual conditions. What happened, and can those
> conditions be recreated?

## Status

RETRACE is pre-alpha but has a working process-recording and inspection slice:

- `retrace run` launches a command and preserves its arguments;
- `run --working-directory PATH` selects and records the target directory;
- each target leads a process group, and received `SIGINT`/`SIGTERM` signals are
  forwarded to that group and recorded;
- normal exits and signal termination are returned to the caller;
- launch failure is distinguished from a target that exits with status 127;
- stdout and stderr are collected concurrently through separate pipes and
  forwarded to the caller;
- `run --output TRACE` writes an exclusive, user-only v1.0 trace containing
  command metadata, lifecycle events, and captured stream chunks;
- `retrace inspect TRACE` renders a bounded, escaped timeline, including valid
  unknown event types;
- `retrace validate TRACE` checks the v1.0 structure without loading the whole
  event stream into memory; and
- each ordinary run locates and loads `libretrace_runtime.so` with `LD_PRELOAD`,
  preserves caller preload entries, validates its bounded Unix-domain channel,
  and records `runtime.handshake`; `--no-runtime` disables this path; and
- dependency-free tests exercise the CLI, process and trace layers, plus C
  stream and runtime fixtures.

Libc-operation instrumentation, time limits, trace export, and fault injection
are not implemented yet. Static, setuid, or otherwise loader-restricted targets
cannot be instrumented; RETRACE reports a missing required handshake after
preserving their observed lifecycle evidence. A successful validation means
that the bytes are structurally valid v1.0; because v1.0 has no footer or
checksum, it does not prove that a run was finalized or that its contents are
authentic.

RETRACE is open source under the [MIT License](LICENSE).

## Product direction

The current runner can execute a target:

```bash
./build/dev/bin/retrace run -- /bin/echo "hello from RETRACE"
./build/dev/bin/retrace run -- python3 -c 'print("hello from Python")'
./build/dev/bin/retrace run --output /tmp/example.rtc -- /bin/echo recorded
./build/dev/bin/retrace run --working-directory /tmp -- /bin/pwd
./build/dev/bin/retrace run --no-runtime -- /bin/echo recorder-only
```

Recorded traces can be inspected or structurally validated:

```bash
./build/dev/bin/retrace inspect /tmp/example.rtc
./build/dev/bin/retrace validate /tmp/example.rtc
```

The v0.1 process-recorder slice is implemented. The v0.2 work now includes a
standalone C17 library, versioned handshake, and a supervisor-owned channel that
automatically loads the runtime into supported dynamic targets, validates the
handshake, and records it. Operation interposition remains the next v0.2 slice.

Later v0.2 work will use the loaded runtime to observe selected libc operations;
v0.3 will control them with bounded fault rules:

```bash
# Planned for v0.3; not implemented yet.
retrace run \
  --fail 'open:/tmp/cache/*:EACCES' \
  --delay 'connect:127.0.0.1:5432:500ms' \
  -- ./example-server
```

RETRACE will not claim deterministic replay, complete syscall visibility, or
debugger-level control. When recording and fault injection ship, it will
recreate selected conditions and record only what it actually observed or
injected.

## Build and run

Requirements:

- Linux
- CMake 3.25 or newer
- Ninja
- A C17 and C++20 compiler (GCC or Clang)

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/bin/retrace version
./build/dev/bin/retrace run -- /bin/echo hello
```

No third-party runtime or test dependencies are used at this stage.

## Documentation

- [Documentation map](docs/README.md)
- [Architecture](docs/architecture.md)
- [Product goals and scope](docs/product.md)
- [Development design](docs/development.md)
- [CLI design](docs/cli.md)
- [Trace format](docs/trace-format.md)
- [Recorder examples](docs/examples.md)
- [Fault rules](docs/fault-rules.md)
- [Security model](docs/security.md)
- [Roadmap](docs/roadmap.md)
- [v0.1 release checklist](docs/release-checklist.md)
- [C and C++ learning guide](docs/learning-c-and-cpp.md)
- [Contributing](CONTRIBUTING.md)

## Safety warning

Trace data may contain command arguments, paths, standard output, and standard
error. Any of those can contain secrets. Recording currently requires an
explicit `--output` path; review every trace before sharing it. See
[docs/security.md](docs/security.md) for the full safety model.

## Scope

RETRACE is Linux-first and does not initially aim to provide kernel modules,
eBPF instrumentation, source-level debugging, profiling, container isolation,
distributed tracing, or multi-host coordination. Small useful releases take
priority over speculative infrastructure.

## License

RETRACE is available under the [MIT License](LICENSE).
