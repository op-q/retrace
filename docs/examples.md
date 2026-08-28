# Recorder examples

The two small C programs under `examples/` exercise the completed v0.1 recorder
without using private or production data. They are built by the normal CMake
presets.

## Normal exit

The normal example writes one line to each stream and exits with status zero:

```bash
./build/dev/bin/retrace run \
  --output /tmp/retrace-normal.rtc \
  --working-directory /tmp \
  -- "$(pwd)/build/dev/bin/retrace_example_normal"
./build/dev/bin/retrace inspect /tmp/retrace-normal.rtc
./build/dev/bin/retrace validate /tmp/retrace-normal.rtc
```

The trace contains start, exec, and `runtime.handshake` events, separate stdout
and stderr chunks, and a final `process.exit` event. The selected `/tmp`
directory is stored in the trace header.

## Intentional crash

The crashing example prints a diagnostic and calls `raise(SIGSEGV)`. Using
`raise()` makes signal termination deliberate; the example does not manufacture
undefined behavior with an invalid memory access.

```bash
./build/dev/bin/retrace run \
  --output /tmp/retrace-crash.rtc \
  -- ./build/dev/bin/retrace_example_crash
status=$?
./build/dev/bin/retrace inspect /tmp/retrace-crash.rtc
printf 'run status: %s\n' "$status"
```

On Linux, the wrapper reports signal 11 and returns shell-style status 139. The
trace includes the runtime handshake and ends with `process.signal signal=11`.
Trace files can contain sensitive arguments and output; review and remove these
`/tmp` examples when finished.
