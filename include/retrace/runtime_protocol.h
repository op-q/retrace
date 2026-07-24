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
};

#endif
