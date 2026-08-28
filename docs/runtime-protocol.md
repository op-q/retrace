# Runtime event protocol

This document defines the live, local contract between the injected C runtime
and the RETRACE supervisor. It is separate from the on-disk trace format:
runtime messages are validated and translated before any trace frame is written.

## Current implementation boundary

Ordinary `retrace run` commands locate `libretrace_runtime.so` beside the build
or installed CLI and prepend its absolute path to `LD_PRELOAD`. Caller preload
entries remain after RETRACE's entry. `--no-runtime` disables the channel and
leaves `LD_PRELOAD` unchanged.

When the library loads, it looks for `RETRACE_RUNTIME_EVENT_FD`, parses that
value as an inherited file descriptor, and sends one handshake frame. The
descriptor is expected to name a connected Unix-domain `SOCK_SEQPACKET` socket.

If the setting is absent, empty, malformed, overflowing, names a standard
descriptor, or refers to an unavailable peer, the runtime continues without
instrumentation. The constructor preserves its incoming `errno` and never emits
a `SIGPIPE` for a closed channel.

The supervisor creates a nonblocking, close-on-exec socket pair before `fork()`.
It closes the unused endpoint on each side, clears close-on-exec only for the
target endpoint, replaces any caller-provided `RETRACE_RUNTIME_EVENT_FD`, and
uses `execvpe()` with the rebuilt environment. The receiver validates each
complete packet before translating a handshake into `runtime.handshake` trace
evidence.

The runtime restores `FD_CLOEXEC` after entering the first target image. Current
instrumentation is scoped to that image: a later `exec` inherits `LD_PRELOAD`
but not the channel, so its constructor fails open rather than emitting a
duplicate session handshake. Broader per-process runtime identity belongs to
later descendant tracking.

Automatic loading requires exactly one handshake. Static, setuid, and other
loader-restricted targets may still run, but RETRACE reports instrumentation as
unavailable after observing their result. Channel-only process-API calls keep
the handshake optional for focused transport tests.

The runtime interposes `open`, `open64`, `openat`, `openat64`, and `close`, and
reports each completed call as an operation message. `connect` and read/write
metadata remain later v0.2 work.

## Frame header

Every integer is unsigned little-endian. The fixed header is 16 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | magic `RTRN` |
| 4 | 2 | protocol major version |
| 6 | 2 | protocol minor version |
| 8 | 2 | message type |
| 10 | 2 | flags; zero in version 1.0 |
| 12 | 4 | payload byte count |

Version 1.0 limits payloads to 64 KiB. The current receiver rejects a bad magic,
unsupported major or newer minor version, nonzero flags, an unknown message
type, duplicate handshakes, a declared/received size mismatch, a truncated
packet, or a payload larger than the limit before decoding it.

`handshake` is message type `1` and currently has an empty payload. Its presence
proves only that the runtime loaded far enough to send the frame; it does not
prove that every requested interposition hook is available.

## Operation messages

`operation` is message type `2`. Its payload is a fixed common prefix followed by
a tail chosen by the operation identifier, so later socket and read/write
operations can reuse the prefix decoder without reinterpreting the fields every
operation shares.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | sequence number |
| 8 | 8 | `CLOCK_MONOTONIC` completion time in nanoseconds |
| 16 | 8 | call duration in nanoseconds |
| 24 | 4 | thread identifier from `gettid` |
| 28 | 2 | operation identifier |
| 30 | 2 | operation flags |

Operation identifiers are `1` for `open`, `2` for `openat`, and `3` for `close`.
A decoder that does not recognize one must reject the frame rather than guess at
its tail layout, and the supervisor does. Operation flag `0x1` marks a truncated
path; it is unrelated to the frame header's flags field, which stays zero.

Every operation defined in version 1.0 carries the file tail:

| Offset | Size | Field |
| ---: | ---: | --- |
| 32 | 8 | `i64` return value |
| 40 | 4 | saved `errno`, meaningful only when the return value is negative |
| 44 | 4 | `i32` descriptor: a successful open result, or a close argument |
| 48 | 4 | `i32` directory descriptor, meaningful only for `openat` |
| 52 | 4 | open flags |
| 56 | 4 | creation mode |
| 60 | 4 | path size |
| 64 | variable | path bytes, without a terminator, at most 4096 |

The supervisor validates every field against the declared payload size before
using it. A target controls these bytes, so a length it supplies is never
trusted to address memory.

The sequence number counts operations the runtime attempted to report, not
operations the supervisor received. Because the channel is nonblocking and
bounded, a full socket drops frames, and a gap in this counter is the only
evidence that loss occurred. The supervisor compares the highest number it saw
against the number of packets it received: each thread takes its sequence before
its own send, so two threads can deliver 6 before 5 without either being lost,
and a running "expected next" counter would misreport ordinary concurrency as
loss. A nonzero result is recorded as `runtime.operations_dropped` rather than
presenting an incomplete record as complete.

Three safety properties govern the hooks themselves:

- each hook resolves its real implementation with `dlsym(RTLD_NEXT, ...)`, falls
  back to the raw syscall if resolution fails, and restores the caller's `errno`,
  so a target keeps working even when instrumentation cannot;
- a path is read only after the real call accepted the pointer, so a target that
  passed an unmapped address receives `EFAULT` rather than a fault the runtime
  introduced; and
- closing the channel descriptor disables emission instead of being recorded.
  The kernel reuses the lowest free descriptor, so a later target open could
  otherwise receive protocol frames into its own file.

## Transport rules

One `send()` call carries one complete frame. `SOCK_SEQPACKET` preserves frame
boundaries, and the runtime uses `MSG_NOSIGNAL` so loss of the supervisor does
not terminate the target. `MSG_DONTWAIT` prevents a full or misconfigured channel
from blocking target startup or slowing a recorded call. The runtime does not
close the inherited descriptor; the operation hooks reuse it in the current
target image.

The environment variable is configuration metadata, not a secret. When runtime
loading is enabled, the supervisor removes every caller-provided value and adds
exactly one descriptor for its target endpoint. With `--no-runtime`, it removes
the value without adding an endpoint. Other RETRACE-owned descriptors remain
close-on-exec. When the direct target exits, the supervisor drains only messages
immediately available and closes its endpoint.
