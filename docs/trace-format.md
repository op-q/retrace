# Trace format

The v1.0 writer, streaming reader, inspector, and validator described here are
implemented. Compatibility tests fix the current byte order, metadata sequence,
event identifiers, bounds, frame boundaries, and malformed-input behavior.

## Requirements

The format must support sequential writes, streaming reads, versioning, bounded
memory use, extension, and recovery after a target or RETRACE crash. It must not
require a footer before complete earlier events can be read.

```text
[trace header]
[event frame]
[event frame]
...
```

Version 1.0 has no footer or checksum. All multibyte integers are unsigned and
little-endian. Sizes count bytes, not characters. The maximum header payload and
maximum individual event payload are both 1 MiB.

## Trace header

The file begins with this fixed 16-byte prefix:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | magic bytes `RETRACE` followed by `0x00` |
| 8 | 2 | major version, currently `1` |
| 10 | 2 | minor version, currently `0` |
| 12 | 4 | header payload size |

The current reader accepts major version `1` only when the minor version is no
greater than the minor version it implements. Because the current implementation
is 1.0, it accepts exactly 1.0 and rejects every other version as unsupported.
This is deliberately conservative: a future minor version is not assumed to be
compatible before its changes are understood.

The bounded header payload then stores fields in this sequence:

| Sequence | Encoding | Field |
| ---: | --- | --- |
| 1 | `u64` | Unix realtime base in nanoseconds |
| 2 | `u64` | Linux monotonic-clock base in nanoseconds |
| 3 | byte string | RETRACE version |
| 4 | byte string | operating-system identifier |
| 5 | byte string | architecture identifier |
| 6 | byte string | working directory |
| 7 | `u32` | target argument count |
| 8 | repeated byte string | each target argument in order |

A byte string is a `u32` length followed by exactly that many bytes. It has no
terminator and is not required to be valid UTF-8. The format can therefore carry
embedded NUL and other non-text bytes, although command arguments supplied by
the current `run` command follow the NUL-terminated Linux process ABI and cannot
contain an embedded NUL. The textual version, operating-system, and architecture
fields use their ordinary ASCII or UTF-8 representations.

The argument count is between 1 and 4096 inclusive. This explicit bound prevents
a malicious header full of empty arguments from multiplying a 1 MiB byte limit
into an excessive number of container allocations.

Environment variables are not copied wholesale. A future explicit allowlist may
store selected values or fingerprints, but the default is no environment
capture.

## Event frames

Each event has a fixed 28-byte prefix followed by its bounded payload:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | frame size after this field: `24 + payload_size` |
| 4 | 2 | event type |
| 6 | 2 | flags, currently zero |
| 8 | 8 | nanoseconds since the header's monotonic base |
| 16 | 4 | process identifier |
| 20 | 4 | thread identifier, currently zero |
| 24 | 4 | payload size |
| 28 | variable | payload bytes |

The redundant frame and payload sizes must agree. The reader rejects impossible
frames and preserves otherwise valid unknown event types as opaque payloads.
The flags field must be zero in v1.0, including for unknown event types.

Frames are appended in nondecreasing monotonic-offset order. Equal timestamps
are valid when multiple observations occur within the clock's resolution.

If a frame write fails, the writer closes its descriptor and does not append
later frames. A reader can therefore keep every complete frame before an
incomplete final frame. This protects the parse boundary after a target or
RETRACE crash; it does not promise durability across power loss or storage
failure.

The CLI explicitly finishes the writer before reporting recording success so a
`close(2)` error can be surfaced. This still does not call `fsync(2)` or promise
durable storage. Writer calls are single-threaded in the current supervisor and
must be externally serialized by any other caller so frame writes cannot
interleave.

The runtime-to-supervisor transport may share event concepts with the trace, but
the in-process transport layout and on-disk format are separate contracts. Raw
C struct bytes are not portable serialization because padding, alignment, and
endianness can differ.

## v1.0 event model

| ID | Name | Payload |
| ---: | --- | --- |
| 1 | `process.start` | empty |
| 2 | `process.exec` | empty |
| 3 | `stdout.chunk` | captured bytes |
| 4 | `stderr.chunk` | captured bytes |
| 5 | `process.exit` | `u32` exit status |
| 6 | `process.signal` | `u32` signal number |
| 7 | `process.launch_failure` | `u32` saved `errno` |
| 8 | `signal.receive` | `u32` signal number |

`process.start` records the child created by `fork()`. `process.exec` is emitted
when the close-on-exec launch-status pipe reaches EOF without reporting an
`execvp()` error. A reported `execvp()` failure instead produces
`process.launch_failure`, so it cannot be confused with a target that exits with
status 127. The EOF observation normally follows successful process-image
replacement, but it cannot prove that the child reached target program entry:
an unusual child termination before `execvp()` can also close the pipe without
an error payload.

Chunk timestamps record supervisor observation order. Separate stdout and stderr
pipes preserve byte order within each stream, but cannot prove exact write order
between streams when both are already readable.

`signal.receive` records a `SIGINT` or `SIGTERM` received by RETRACE and
successfully forwarded to the target process group. It is distinct from
`process.signal`, which records that the direct target was ultimately terminated
by a signal. A target may handle a forwarded signal and exit normally.

## Planned event extensions

Later process and signal work may add `process.fork` and `signal.inject`.

File events, introduced with the C runtime:

- `file.open`, `file.close`, `file.read`, `file.write`

Early file events store metadata such as path, descriptor, requested and actual
byte counts, return value, `errno`, and duration—not file contents.

Socket events:

- `socket.create`, `socket.connect`, `socket.accept`
- `socket.send`, `socket.receive`, `socket.close`

Initial implementation should focus on `connect` rather than claiming full
socket visibility.

Fault events:

- `fault.match`, `fault.delay`, `fault.fail`
- `fault.signal`, `fault.kill`

Every injected action requires a corresponding event.

## Planned JSON export

Binary storage will be exportable to a reviewable representation:

```json
{
  "trace_id": "7df2c2a1",
  "command": ["./example-server"],
  "started_at": "2026-07-20T09:15:21.442+02:00",
  "events": [
    {
      "offset_ns": 0,
      "type": "process.start",
      "pid": 4210
    }
  ]
}
```

## Reader hardening

Trace files are untrusted input. The reader requires a regular file and checks
the exact 16-byte prefix before decoding a header. It applies the 1 MiB bounds
before allocating, limits the argument count to 4096, uses checked cursor
arithmetic for every metadata field, and rejects unconsumed header bytes.

For each event it checks the bounded frame length before allocating, verifies
the redundant payload length and zero flags, and requires nondecreasing
timestamps. It also enforces the v1.0 payload schemas: `process.start` and
`process.exec` are empty, stream chunks contain arbitrary bounded bytes, and
exit, signal, launch-failure, and signal-receive payloads are exactly one
little-endian `u32`.
Unknown event identifiers remain valid when their surrounding frame is valid;
the reader exposes their type, thread ID, and opaque bounded payload without
assigning semantics.

Clean EOF, an incomplete final frame, a malformed trace, an unsupported version,
and an operating-system I/O error are distinct results. `inspect` renders each
complete preceding event before reporting an incomplete final frame. It quotes,
escapes, and caps untrusted byte previews rather than writing raw payload bytes
to the terminal. `validate` consumes the same stream without rendering payloads.

Tests use independently encoded inputs to verify reader compatibility, bounds,
timeline output, every representative truncation point, corrupt payloads, and
unknown future event handling. JSON export remains planned.

## Structural limits

Version 1.0 deliberately has no footer, checksum, signature, or finalization
record. Reaching clean EOF proves only that the decoded bytes end on a complete
frame boundary. It does not prove that the writer finalized the run, that a
frame-aligned suffix was not lost, that storage made every write durable, or
that the trace is authentic. Structural validation also does not require a
particular lifecycle-event sequence. Callers must not interpret
`trace is valid` as evidence of a complete or trusted recording.
