// Target-side runtime-channel fixture. One mode loads the real shared runtime;
// the remaining modes send deliberately malformed packets to harden the C++
// receiver against untrusted target input.

#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "retrace/runtime_protocol.h"

#ifndef RETRACE_RUNTIME_LIBRARY_PATH
#error "RETRACE_RUNTIME_LIBRARY_PATH must name the runtime shared library"
#endif

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

static int runtime_descriptor(void) {
  const char* const text = getenv(RETRACE_RUNTIME_EVENT_FD_ENV);
  if (text == NULL || text[0] == '\0') {
    return -1;
  }

  errno = 0;
  char* end = NULL;
  const long value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < 3L ||
      value > (long)INT_MAX) {
    return -1;
  }
  return (int)value;
}

static void make_handshake(unsigned char* const frame) {
  memset(frame, 0, (size_t)RETRACE_RUNTIME_FRAME_HEADER_SIZE);
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
}

static int send_packet(const int descriptor, const void* const bytes,
                       const size_t size) {
  ssize_t result = -1;
  do {
    result = send(descriptor, bytes, size, MSG_DONTWAIT | MSG_NOSIGNAL);
  } while (result < 0 && errno == EINTR);
  return result == (ssize_t)size ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int load_runtime(void) {
  void* const handle = dlopen(RETRACE_RUNTIME_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL);
  if (handle == NULL) {
    return EXIT_FAILURE;
  }
  return dlclose(handle) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(const int argument_count, char* const arguments[]) {
  if (argument_count != 2) {
    return EXIT_FAILURE;
  }

  if (strcmp(arguments[1], "load-runtime") == 0) {
    return load_runtime();
  }

  const int descriptor = runtime_descriptor();
  if (descriptor < 0) {
    return EXIT_FAILURE;
  }
  if (strcmp(arguments[1], "no-handshake") == 0) {
    return EXIT_SUCCESS;
  }

  unsigned char frame[RETRACE_RUNTIME_FRAME_HEADER_SIZE];
  make_handshake(frame);

  if (strcmp(arguments[1], "bad-magic") == 0) {
    frame[RETRACE_RUNTIME_FRAME_MAGIC_OFFSET] = (unsigned char)'X';
  } else if (strcmp(arguments[1], "unsupported-major") == 0) {
    store_u16_le(&frame[RETRACE_RUNTIME_FRAME_MAJOR_OFFSET],
                 RETRACE_RUNTIME_PROTOCOL_MAJOR + UINT16_C(1));
  } else if (strcmp(arguments[1], "unsupported-minor") == 0) {
    store_u16_le(&frame[RETRACE_RUNTIME_FRAME_MINOR_OFFSET],
                 RETRACE_RUNTIME_PROTOCOL_MINOR + UINT16_C(1));
  } else if (strcmp(arguments[1], "nonzero-flags") == 0) {
    store_u16_le(&frame[RETRACE_RUNTIME_FRAME_FLAGS_OFFSET], UINT16_C(1));
  } else if (strcmp(arguments[1], "unknown-type") == 0) {
    store_u16_le(&frame[RETRACE_RUNTIME_FRAME_TYPE_OFFSET], UINT16_C(99));
  } else if (strcmp(arguments[1], "payload-mismatch") == 0) {
    store_u32_le(&frame[RETRACE_RUNTIME_FRAME_PAYLOAD_SIZE_OFFSET], UINT32_C(1));
  } else if (strcmp(arguments[1], "short-frame") == 0) {
    return send_packet(descriptor, frame,
                       (size_t)RETRACE_RUNTIME_FRAME_HEADER_SIZE - 1U);
  } else if (strcmp(arguments[1], "oversized") == 0) {
    static unsigned char oversized[RETRACE_RUNTIME_FRAME_HEADER_SIZE +
                                   RETRACE_RUNTIME_FRAME_MAX_PAYLOAD_SIZE +
                                   UINT32_C(1)];
    memcpy(oversized, frame, (size_t)RETRACE_RUNTIME_FRAME_HEADER_SIZE);
    store_u32_le(&oversized[RETRACE_RUNTIME_FRAME_PAYLOAD_SIZE_OFFSET],
                 RETRACE_RUNTIME_FRAME_MAX_PAYLOAD_SIZE + UINT32_C(1));
    return send_packet(descriptor, oversized, sizeof(oversized));
  } else if (strcmp(arguments[1], "duplicate-handshake") == 0) {
    if (send_packet(descriptor, frame, sizeof(frame)) != EXIT_SUCCESS) {
      return EXIT_FAILURE;
    }
    return send_packet(descriptor, frame, sizeof(frame));
  } else {
    return EXIT_FAILURE;
  }

  return send_packet(descriptor, frame, sizeof(frame));
}
