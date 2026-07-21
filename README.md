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
- dependency-free tests exercise the CLI, process and trace layers, and a C
  stream fixture.

Process groups, signal forwarding, trace export, and fault injection are not
implemented yet. A successful validation means that the bytes are structurally
valid v1.0; because v1.0 has no footer or checksum, it does not prove that a run
was finalized or that its contents are authentic.

RETRACE is open source under the [MIT License](LICENSE).

## Product direction

The current runner can execute a target:

```bash
./build/dev/bin/retrace run -- /bin/echo "hello from RETRACE"
./build/dev/bin/retrace run -- python3 -c 'print("hello from Python")'
./build/dev/bin/retrace run --output /tmp/example.rtc -- /bin/echo recorded
```

Recorded traces can be inspected or structurally validated:

```bash
./build/dev/bin/retrace inspect /tmp/example.rtc
./build/dev/bin/retrace validate /tmp/example.rtc
```

The next v0.1 engineering milestone is process-group creation and signal
forwarding.

Later releases will add a small C runtime loaded with `LD_PRELOAD` so selected
libc operations can be observed and controlled:

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

- [Architecture](docs/architecture.md)
- [Product goals and scope](docs/product.md)
- [Development design](docs/development.md)
- [CLI design](docs/cli.md)
- [Trace format](docs/trace-format.md)
- [Fault rules](docs/fault-rules.md)
- [Security model](docs/security.md)
- [Roadmap](docs/roadmap.md)
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
