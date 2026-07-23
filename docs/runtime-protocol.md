# Runtime event protocol

This document defines the live, local contract between the injected C runtime
and the RETRACE supervisor. It is separate from the on-disk trace format:
runtime messages are validated and translated before any trace frame is written.

## Current implementation boundary

The first v0.2 slice builds `libretrace_runtime.so`. When the library loads, it
looks for `RETRACE_RUNTIME_EVENT_FD`, parses that value as an inherited file
descriptor, and sends one handshake frame. The descriptor is expected to name a
connected Unix-domain `SOCK_SEQPACKET` socket.

If the setting is absent, empty, malformed, overflowing, names a standard
descriptor, or refers to an unavailable peer, the runtime continues without
instrumentation. The constructor preserves its incoming `errno` and never emits
a `SIGPIPE` for a closed channel.

The supervisor does not create or consume this channel yet, and `retrace run`
does not load the runtime in this slice. libc interposition and operation events
remain later v0.2 work.

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

Version 1.0 limits payloads to 64 KiB. A receiver must reject a bad magic,
unsupported major version, nonzero flags, an unknown required message type, or a
payload larger than the limit before allocating or decoding it.

`handshake` is message type `1` and currently has an empty payload. Its presence
proves only that the runtime loaded far enough to send the frame; it does not
prove that every requested interposition hook is available.

## Transport rules

One `send()` call carries one complete frame. `SOCK_SEQPACKET` preserves frame
boundaries, and the runtime uses `MSG_NOSIGNAL` so loss of the supervisor does
not terminate the target. `MSG_DONTWAIT` prevents a full or misconfigured channel
from blocking target startup. The runtime does not close the inherited
descriptor; later operation hooks will reuse it.

The environment variable is configuration metadata, not a secret. Future
supervisor integration must remove or replace a caller-provided value, pass only
the intended descriptor, and close unrelated inherited descriptors.
