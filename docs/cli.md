# Command-line interface

`version`, help, and the basic `run -- COMMAND [ARGS...]` path are implemented.
Run options, trace creation, inspection, export, and validation remain planned.
Examples are labeled where they describe future behavior.

## Commands

```text
retrace run
retrace inspect
retrace export
retrace validate
retrace scenario
retrace version
```

## Run a target

```bash
retrace run -- COMMAND [ARGS...]
```

Everything after `--` belongs to the target. RETRACE currently launches the
command with `fork()` and `execvp()`, waits with `waitpid()`, and returns the
target's exit code. A target killed by a signal returns the usual shell-style
`128 + signal` status. A missing executable returns RETRACE code `5`.

Stdout and stderr currently pass directly to the same terminal. RETRACE does
not yet create a trace, create a process group, forward signals, or capture
streams.

Planned options include:

```text
--output PATH
--scenario PATH
--fail RULE
--delay RULE
--kill-after DURATION
--signal SIGNAL
--seed NUMBER
--capture-stdout
--capture-stderr
--no-runtime
--working-directory PATH
--environment KEY=VALUE
--inherit-environment KEY
--verbose
```

A completed v0.1 run will create a session, capture configured streams, record
process metadata, write all recoverable trace data, and print an exit summary.
A crash must not erase already completed frames.

## Inspect a trace

```bash
retrace inspect trace.rtc [OPTIONS]
```

Planned filters:

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

Example output:

```text
TRACE 7df2c2a1
COMMAND ./example-server
DURATION 1.204s
EXIT signal=SIGTERM code=143

     0.000 ms  process.start    pid=4210
    13.821 ms  file.open       path=/etc/example/config.json result=ok
  1198.441 ms  signal.receive  signal=SIGTERM
  1203.992 ms  process.exit    code=143
```

## Export and validate

```bash
retrace export trace.rtc --format json
retrace validate trace.rtc
```

Export formats may include JSON, JSON Lines, and later Chrome Trace Event.
Validation checks the header, version, frame bounds, payload limits, checksums
when present, unknown event types, and an incomplete final frame.

## Signals and time limits

```bash
retrace run --kill-after 5s --signal SIGTERM -- ./server
```

RETRACE will forward `SIGINT` and `SIGTERM` to the target process group, record
whether a signal came from the user or a scenario, allow a bounded graceful-exit
period, and escalate only when configured.

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
