# Trace format

This is the design baseline for the v0.1 format, not a frozen wire
specification. Concrete integer encodings and limits will be fixed alongside
writer/reader compatibility tests.

## Requirements

The format must support sequential writes, streaming reads, versioning, bounded
memory use, extension, and recovery after a target or RETRACE crash. It must not
require a footer before complete earlier events can be read.

```text
[trace header]
[event frame]
[event frame]
...
[optional footer]
```

## Trace header

The header is expected to contain:

- magic bytes and format version;
- RETRACE version;
- creation time and host architecture;
- operating-system identifier;
- target command and working directory;
- scenario identifier and deterministic seed, when present; and
- monotonic and wall-clock bases.

Environment variables are not copied wholesale. A future explicit allowlist may
store selected values or fingerprints, but the default is no environment
capture.

## Event frames

A frame will contain a bounded frame length, event type, monotonic timestamp,
process and thread identifiers, payload length, payload, and possibly a
checksum. All multibyte encodings, byte order, size limits, and string rules
must be explicit before the format is released.

The runtime-to-supervisor transport may share event concepts with the trace, but
the in-process transport layout and on-disk format are separate contracts. Raw
C struct bytes are not portable serialization because padding, alignment, and
endianness can differ.

## Initial event model

Process events:

- `process.start`, `process.exec`, `process.fork`
- `process.exit`, `process.crash`
- `signal.receive`, `signal.inject`

Stream events:

- `stdout.chunk`, `stderr.chunk`

Large stream data may be stored separately to bound main-event growth.

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

## JSON export

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

Trace files are untrusted input. A reader must validate every size and type,
limit allocations, detect integer overflow, avoid relying on string
termination, reject impossible payloads, and either recover safely or fail
cleanly. Unknown future event types should remain skippable when their frame is
otherwise valid.

Golden trace fixtures will verify reader compatibility, validation, timeline
output, JSON export, truncated final frames, corrupt payloads, and unknown
future event handling.
