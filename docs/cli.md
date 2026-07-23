# Command-line interface

`version`, help, `run -- COMMAND [ARGS...]`, selected working directories,
explicit trace creation, signal forwarding, inspection, and structural
validation are implemented. Export, scenarios, filters, and the remaining run
options are planned. Examples are labeled where they describe future behavior.

## Commands

```text
retrace run [--output TRACE] [--working-directory PATH] -- COMMAND [ARGS...]
retrace inspect TRACE
retrace validate TRACE
retrace version
```

Planned commands include `export` and `scenario`.

`retrace run --help` prints the implemented run options and signal behavior.

## Run a target

```bash
retrace run [--output TRACE] [--working-directory PATH] -- COMMAND [ARGS...]
```

Everything after `--` belongs to the target. RETRACE currently launches the
command with `fork()` and `execvp()`, places it in a new process group, collects
stdout and stderr concurrently through separate pipes, forwards them to its own
corresponding streams, waits with `waitpid()`, and returns the target's exit
code. `SIGINT` and `SIGTERM` received during supervision are forwarded to the
whole target group. A target killed by a signal returns the usual shell-style
`128 + signal` status. A missing executable returns RETRACE code `5`.

`--working-directory PATH` resolves an existing directory before trace creation
or target launch. The child calls `chdir()` before `execvp()`, and the resolved
directory is stored in trace metadata. A relative `--output` path remains
relative to RETRACE's own directory; the supervisor does not change directory.

Without `--output`, RETRACE only relays captured streams. With `--output TRACE`,
it creates a new v1.0 file and records command metadata, `process.start`,
`process.exec`, stdout/stderr chunks, successfully forwarded signals, and the
final exit or signal. A failed
`execvp()` is recorded separately from target exit status 127. `process.exec`
means that the close-on-exec launch-status pipe reached EOF without reporting an
`execvp()` error. It normally follows a successful replacement, but cannot prove
that the child reached target program entry because an unusual pre-`execvp()`
termination can also close the pipe.

Trace creation never overwrites an existing path, does not follow a final
symlink, uses user-only permissions, and occurs before the target starts. Parent
directories must already exist. Because the target writes to pipes, programs
that select buffering based on whether a stream is a terminal may behave
differently.

The direct target is reaped while its streams are collected. If a descendant
keeps an inherited stream open after that target exits, RETRACE drains the bytes
already queued at the exit observation and then closes its read ends; a daemon
cannot keep `run` waiting indefinitely. Descendants receive group-directed
signals, but RETRACE does not yet discover or report their individual lifecycle
events.

Planned options include:

```text
--scenario PATH
--fail RULE
--delay RULE
--kill-after DURATION
--signal SIGNAL
--seed NUMBER
--capture-stdout
--capture-stderr
--no-runtime
--environment KEY=VALUE
--inherit-environment KEY
--verbose
```

A v0.1 run creates a session, captures both streams, records process metadata,
and writes all recoverable trace data. A crash does not erase already completed
frames.

## Inspect a trace

```bash
retrace inspect trace.rtc
```

`inspect` requires exactly one path. It prints header metadata, every complete
event in timestamp order, and a summary. On a complete structural parse, the
summary has `status=structurally-valid`. The current command has no filters or
raw-output mode.

Trace metadata and payloads are bytes, not trusted terminal text. The renderer
quotes them and escapes NUL, newline, carriage return, tab, quotes, backslashes,
and other non-printable bytes. Metadata previews are limited to 160 input bytes,
stream and unknown-event previews to 64 input bytes, and the command line to the
first 16 arguments. A valid event identifier not known to this build is shown as
`unknown(ID)` with its thread ID, byte count, and escaped preview.

Representative output:

```text
TRACE version=1.0
RECORDER "0.1.0"
SYSTEM "Linux" architecture="x86_64"
COMMAND "/bin/echo" "hello"
WORKING_DIRECTORY "/tmp"
CREATED_UNIX_NS 1784541600000000000
EVENTS
       0.012 ms  process.start           pid=4210
       0.038 ms  process.exec            pid=4210
       0.174 ms  stdout.chunk            pid=4210 bytes=6 preview="hello\n"
       0.281 ms  process.exit            pid=4210 code=0
SUMMARY events=4 duration=0.281 ms result=exit(0) status=structurally-valid
```

If only the final event frame is incomplete, `inspect` still prints the header,
every preceding complete event, and a summary with `status=incomplete`. It also
writes a diagnostic to stderr and returns code `4`. Other malformed events stop
inspection after any timeline entries already rendered and also return code `4`.
An output-stream failure observed while writing or flushing normal command
output returns code `1` rather than reporting a false success.

Planned filters include:

```text
--type TYPE
--pid PID
--tid TID
--errors-only
--from DURATION
--to DURATION
--summary
--raw
```

## Export and validate

```bash
retrace validate trace.rtc
```

`validate` requires exactly one path and consumes the event stream without
rendering payloads. Success writes exactly `trace is valid` followed by a
newline and returns `0`. It checks the magic, supported version, header and
argument bounds, exact metadata layout, frame and payload bounds, zero v1.0
flags, known-event payload shapes, and nondecreasing timestamps. Structurally
valid unknown event identifiers are accepted.

Malformed, unsupported, truncated, or non-regular traces return `4`.
Operating-system open or read errors, such as a missing or permission-denied
path, return `1`; wrong command arguments return `2`. Version 1.0 has no footer
or checksum, so validation establishes structural conformance only. Clean EOF
does not prove that a recording was finalized, that a frame-aligned suffix was
not lost, or that its contents are authentic.

An observed failure to write or flush the validation success message also
returns `1`.

Export is planned, not implemented:

```bash
retrace export trace.rtc --format json
```

Possible export formats include JSON, JSON Lines, and later Chrome Trace Event.

## Signals and time limits

```bash
retrace run --kill-after 5s --signal SIGTERM -- ./server
```

RETRACE currently receives `SIGINT` and `SIGTERM` through a pollable signal
source and forwards them to the target process group. A successful forwarding
is recorded as `signal.receive`; the target's eventual signal termination is a
separate `process.signal` event. Source classification, `--kill-after`, a
graceful-exit period, escalation, and scenario-driven signals remain planned.

## Exit codes

The initial RETRACE-specific codes are:

```text
0  RETRACE command succeeded
1  internal failure
2  invalid command-line arguments
3  invalid scenario
4  trace format error
5  target failed to launch
```

For `run`, ordinary target exits mirror the target's status. RETRACE usage,
internal, and launch failures also use the codes above, so a target can
coincidentally return the same number. RETRACE's diagnostic distinguishes its
own failure from an ordinary target exit. This behavior is covered by process
and CLI tests.

## Diagnostics

`--verbose` may report created pipes, runtime path, target PID, trace
destination, runtime handshake, event and drop counts, and finalization state.
RETRACE diagnostics must never be mixed into captured target streams.
