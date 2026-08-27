#ifndef RETRACE_RUNTIME_PROTOCOL_H
#define RETRACE_RUNTIME_PROTOCOL_H

// C-compatible live protocol shared by the injected C17 runtime and C++20
// supervisor. These constants describe serialized bytes, never a copied struct.

#include <stdint.h>

// The supervisor replaces any caller-provided value with the one descriptor
// intentionally inherited by the target process.
#define RETRACE_RUNTIME_EVENT_FD_ENV "RETRACE_RUNTIME_EVENT_FD"

#define RETRACE_RUNTIME_PROTOCOL_MAGIC_0 ((uint8_t)'R')
#define RETRACE_RUNTIME_PROTOCOL_MAGIC_1 ((uint8_t)'T')
#define RETRACE_RUNTIME_PROTOCOL_MAGIC_2 ((uint8_t)'R')
#define RETRACE_RUNTIME_PROTOCOL_MAGIC_3 ((uint8_t)'N')

#define RETRACE_RUNTIME_PROTOCOL_MAJOR UINT16_C(1)
#define RETRACE_RUNTIME_PROTOCOL_MINOR UINT16_C(0)

#define RETRACE_RUNTIME_FRAME_HEADER_SIZE UINT32_C(16)
#define RETRACE_RUNTIME_FRAME_MAX_PAYLOAD_SIZE (UINT32_C(64) * UINT32_C(1024))

// Named offsets keep the C encoder and C++ decoder independent from compiler
// padding, alignment, and host byte order.
#define RETRACE_RUNTIME_FRAME_MAGIC_OFFSET UINT32_C(0)
#define RETRACE_RUNTIME_FRAME_MAJOR_OFFSET UINT32_C(4)
#define RETRACE_RUNTIME_FRAME_MINOR_OFFSET UINT32_C(6)
#define RETRACE_RUNTIME_FRAME_TYPE_OFFSET UINT32_C(8)
#define RETRACE_RUNTIME_FRAME_FLAGS_OFFSET UINT32_C(10)
#define RETRACE_RUNTIME_FRAME_PAYLOAD_SIZE_OFFSET UINT32_C(12)

// C17 cannot fix an enum's underlying width. Protocol code serializes this
// value explicitly as uint16_t and never copies the enum object itself.
enum retrace_runtime_message_type {  // NOLINT(performance-enum-size)
  RETRACE_RUNTIME_MESSAGE_HANDSHAKE = 1,
  RETRACE_RUNTIME_MESSAGE_OPERATION = 2,
};

// Operation payload layout
//
// An operation message payload is a fixed common prefix followed by a tail
// chosen by the operation identifier. Splitting the two lets later socket and
// read/write operations reuse the prefix decoder without reinterpreting the
// fields that every operation shares.

#define RETRACE_RUNTIME_OPERATION_SEQUENCE_OFFSET UINT32_C(0)
#define RETRACE_RUNTIME_OPERATION_MONOTONIC_OFFSET UINT32_C(8)
#define RETRACE_RUNTIME_OPERATION_DURATION_OFFSET UINT32_C(16)
#define RETRACE_RUNTIME_OPERATION_THREAD_OFFSET UINT32_C(24)
#define RETRACE_RUNTIME_OPERATION_ID_OFFSET UINT32_C(28)
#define RETRACE_RUNTIME_OPERATION_FLAGS_OFFSET UINT32_C(30)
#define RETRACE_RUNTIME_OPERATION_PREFIX_SIZE UINT32_C(32)

// The sequence number counts operations the runtime attempted to report, not
// operations the supervisor received. Because the channel is nonblocking and
// bounded, a full socket drops frames; a gap in this counter is the only
// evidence that loss occurred, so the supervisor reports the gap rather than
// silently under-reporting the target's behavior.
//
// Timestamps use CLOCK_MONOTONIC so they share a base with the trace header and
// remain comparable to supervisor-observed offsets. Recording the runtime's own
// completion time keeps concurrent operations orderable even when socket
// receive order does not match call order.

// Operation identifiers are serialized as uint16_t. Values are stable across
// minor versions; a decoder that does not recognize one must reject the frame
// rather than guess at its tail layout.
enum retrace_runtime_operation_id {  // NOLINT(performance-enum-size)
  RETRACE_RUNTIME_OPERATION_OPEN = 1,
  RETRACE_RUNTIME_OPERATION_OPENAT = 2,
  RETRACE_RUNTIME_OPERATION_CLOSE = 3,
};

// Operation flags are scoped to one operation and are unrelated to the frame
// header's flags field, which stays zero in version 1.0. A decoder must reject
// any bit outside the defined mask so later additions cannot be misread.
#define RETRACE_RUNTIME_OPERATION_FLAG_PATH_TRUNCATED UINT16_C(0x0001)
#define RETRACE_RUNTIME_OPERATION_FLAGS_DEFINED \
  RETRACE_RUNTIME_OPERATION_FLAG_PATH_TRUNCATED

// File-operation tail, following the common prefix.
//
// `result` is the value the interposed call returned, stored as a two's
// complement int64. `error_number` is meaningful only when `result` is
// negative. `descriptor` repeats a successful open result and carries the
// argument of a close, so a consumer never has to know which field a given
// operation used. `directory` is meaningful only for openat.
#define RETRACE_RUNTIME_FILE_RESULT_OFFSET UINT32_C(32)
#define RETRACE_RUNTIME_FILE_ERRNO_OFFSET UINT32_C(40)
#define RETRACE_RUNTIME_FILE_DESCRIPTOR_OFFSET UINT32_C(44)
#define RETRACE_RUNTIME_FILE_DIRECTORY_OFFSET UINT32_C(48)
#define RETRACE_RUNTIME_FILE_OPEN_FLAGS_OFFSET UINT32_C(52)
#define RETRACE_RUNTIME_FILE_MODE_OFFSET UINT32_C(56)
#define RETRACE_RUNTIME_FILE_PATH_SIZE_OFFSET UINT32_C(60)
#define RETRACE_RUNTIME_FILE_HEADER_SIZE UINT32_C(64)

// Paths are stored without a terminator and are truncated to this bound, which
// matches the Linux PATH_MAX including the NUL that is not stored. A target may
// legally pass a longer path and receive ENAMETOOLONG; the attempt is still
// recorded, marked with the truncation flag. The resulting maximum file payload
// stays far below the 64 KiB frame limit.
#define RETRACE_RUNTIME_FILE_MAX_PATH_SIZE UINT32_C(4096)

#endif
