# RETRACE

[![CI](https://github.com/op-q/retrace/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/op-q/retrace/actions/workflows/ci.yml)

RETRACE is a native Linux process recorder and fault-injection tool. Its goal
is to show important process-boundary behavior and then recreate selected
failure conditions deliberately.

> A program failed under unusual conditions. What happened, and can those
> conditions be recreated?

## Status

RETRACE is pre-alpha but has a working process-execution slice:

- `retrace run` launches a command and preserves its arguments;
- normal exits and signal termination are returned to the caller;
- launch failure is distinguished from a target that exits with status 127; and
- dependency-free tests exercise the CLI, process layer, and a C stream fixture.

The target currently inherits the terminal's stdout and stderr. Trace writing,
stream capture, process groups, and signal forwarding are not implemented yet.

RETRACE is open source under the [MIT License](LICENSE).

## Product direction

The current runner can execute a target:

```bash
./build/dev/bin/retrace run -- /bin/echo "hello from RETRACE"
./build/dev/bin/retrace run -- python3 -c 'print("hello from Python")'
```

The rest of v0.1 will record lifecycle and output, write a crash-tolerant trace,
and render a readable timeline:

```bash
# Planned for v0.1; not implemented yet.
retrace inspect .retrace/traces/latest.rtc
```

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

Future trace data may contain command arguments, paths, standard output, and
standard error. Any of those can contain secrets. Capture features must minimize
collection by default, but users will still need to review traces before sharing
them. See [docs/security.md](docs/security.md) for the full safety model.

## Scope

RETRACE is Linux-first and does not initially aim to provide kernel modules,
eBPF instrumentation, source-level debugging, profiling, container isolation,
distributed tracing, or multi-host coordination. Small useful releases take
priority over speculative infrastructure.

## License

RETRACE is available under the [MIT License](LICENSE).
