#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "retrace/runtime_protocol.h"

static void store_u16_le(unsigned char* const destination, const uint16_t value) {
  destination[0] = (unsigned char)(value & UINT16_C(0xff));
  destination[1] = (unsigned char)((value >> 8U) & UINT16_C(0xff));
}

static void store_u32_le(unsigned char* const destination, const uint32_t value) {
  destination[0] = (unsigned char)(value & UINT32_C(0xff));
  destination[1] = (unsigned char)((value >> 8U) & UINT32_C(0xff));
  destination[2] = (unsigned char)((value >> 16U) & UINT32_C(0xff));
  destination[3] = (unsigned char)((value >> 24U) & UINT32_C(0xff));
}

static int parse_event_descriptor(const char* const text) {
  if (text == NULL || text[0] == '\0') {
    return -1;
  }

  unsigned int value = 0U;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') {
      return -1;
    }
    const unsigned int digit = (unsigned int)(*cursor - '0');
    if (value > ((unsigned int)INT_MAX - digit) / 10U) {
      return -1;
    }
    value = (value * 10U) + digit;
  }

  if (value < 3U) {
    return -1;
  }
  return (int)value;
}

static void emit_handshake(void) {
  const int saved_errno = errno;
  const int descriptor = parse_event_descriptor(getenv(RETRACE_RUNTIME_EVENT_FD_ENV));

  if (descriptor >= 0) {
    unsigned char frame[RETRACE_RUNTIME_FRAME_HEADER_SIZE] = {0U};
    frame[RETRACE_RUNTIME_FRAME_MAGIC_OFFSET + 0U] = RETRACE_RUNTIME_PROTOCOL_MAGIC_0;
    frame[RETRACE_RUNTIME_FRAME_MAGIC_OFFSET + 1U] = RETRACE_RUNTIME_PROTOCOL_MAGIC_1;
    frame[RETRACE_RUNTIME_FRAME_MAGIC_OFFSET + 2U] = RETRACE_RUNTIME_PROTOCOL_MAGIC_2;
    frame[RETRACE_RUNTIME_FRAME_MAGIC_OFFSET + 3U] = RETRACE_RUNTIME_PROTOCOL_MAGIC_3;
    store_u16_le(&frame[RETRACE_RUNTIME_FRAME_MAJOR_OFFSET],
                 RETRACE_RUNTIME_PROTOCOL_MAJOR);
    store_u16_le(&frame[RETRACE_RUNTIME_FRAME_MINOR_OFFSET],
                 RETRACE_RUNTIME_PROTOCOL_MINOR);
    store_u16_le(&frame[RETRACE_RUNTIME_FRAME_TYPE_OFFSET],
                 (uint16_t)RETRACE_RUNTIME_MESSAGE_HANDSHAKE);
    store_u16_le(&frame[RETRACE_RUNTIME_FRAME_FLAGS_OFFSET], UINT16_C(0));
    store_u32_le(&frame[RETRACE_RUNTIME_FRAME_PAYLOAD_SIZE_OFFSET], UINT32_C(0));

    ssize_t result = -1;
    do {
      result = send(descriptor, frame, sizeof(frame), MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (result < 0 && errno == EINTR);
  }

  errno = saved_errno;
}

static void retrace_runtime_start(void) __attribute__((constructor));

static void retrace_runtime_start(void) { emit_handshake(); }
