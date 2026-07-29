// Standalone C tests for the injected runtime constructor. Each case runs in a
// child because a shared-library constructor normally executes once per load
// and because closed/full socket behavior must not terminate the test runner.

#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "retrace/runtime_protocol.h"

#ifndef RETRACE_RUNTIME_LIBRARY_PATH
#error "RETRACE_RUNTIME_LIBRARY_PATH must name the runtime shared library"
#endif

static int failure_count = 0;

static void expect(const int condition, const char* const message) {
  if (condition == 0) {
    fprintf(stderr, "FAIL: %s\n", message);
    ++failure_count;
  }
}

static uint16_t load_u16_le(const unsigned char* const source) {
  return (uint16_t)((uint16_t)source[0] | ((uint16_t)source[1] << 8U));
}

static uint32_t load_u32_le(const unsigned char* const source) {
  return (uint32_t)source[0] | ((uint32_t)source[1] << 8U) |
         ((uint32_t)source[2] << 16U) | ((uint32_t)source[3] << 24U);
}

static int load_runtime(void) {
  void* const handle = dlopen(RETRACE_RUNTIME_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL);
  if (handle == NULL) {
    fprintf(stderr, "runtime dlopen failed: %s\n", dlerror());
    return -1;
  }
  return dlclose(handle);
}

static int wait_for_success(const pid_t child) {
  int status = 0;
  pid_t result = -1;
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  return result == child && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
}

static int set_descriptor_environment(const int descriptor) {
  char text[32];
  const int length = snprintf(text, sizeof(text), "%d", descriptor);
  if (length <= 0 || (size_t)length >= sizeof(text)) {
    return -1;
  }
  return setenv(RETRACE_RUNTIME_EVENT_FD_ENV, text, 1);
}

static void test_handshake_frame(void) {
  int channel[2] = {-1, -1};
  expect(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, channel) == 0,
         "a local sequenced-packet channel is available");
  if (channel[0] < 0 || channel[1] < 0) {
    return;
  }

  const pid_t child = fork();
  expect(child >= 0, "a runtime handshake child can be created");
  if (child < 0) {
    close(channel[0]);
    close(channel[1]);
    return;
  }
  if (child == 0) {
    close(channel[0]);
    if (set_descriptor_environment(channel[1]) < 0 || load_runtime() < 0) {
      _exit(EXIT_FAILURE);
    }
    close(channel[1]);
    _exit(EXIT_SUCCESS);
  }

  close(channel[1]);
  unsigned char frame[RETRACE_RUNTIME_FRAME_HEADER_SIZE + 1U] = {0U};
  ssize_t received = -1;
  do {
    received = recv(channel[0], frame, sizeof(frame), 0);
  } while (received < 0 && errno == EINTR);
  const int receive_errno = errno;
  close(channel[0]);

  if (received != (ssize_t)RETRACE_RUNTIME_FRAME_HEADER_SIZE) {
    fprintf(stderr, "handshake recv returned %zd: %s\n", received,
            strerror(receive_errno));
  }
  expect(received == (ssize_t)RETRACE_RUNTIME_FRAME_HEADER_SIZE,
         "the runtime emits one complete handshake frame");
  if (received == (ssize_t)RETRACE_RUNTIME_FRAME_HEADER_SIZE) {
    expect(frame[0] == RETRACE_RUNTIME_PROTOCOL_MAGIC_0 &&
               frame[1] == RETRACE_RUNTIME_PROTOCOL_MAGIC_1 &&
               frame[2] == RETRACE_RUNTIME_PROTOCOL_MAGIC_2 &&
               frame[3] == RETRACE_RUNTIME_PROTOCOL_MAGIC_3,
           "the handshake contains the runtime protocol magic");
    expect(load_u16_le(&frame[RETRACE_RUNTIME_FRAME_MAJOR_OFFSET]) ==
                   RETRACE_RUNTIME_PROTOCOL_MAJOR &&
               load_u16_le(&frame[RETRACE_RUNTIME_FRAME_MINOR_OFFSET]) ==
                   RETRACE_RUNTIME_PROTOCOL_MINOR,
           "the handshake contains the supported protocol version");
    expect(load_u16_le(&frame[RETRACE_RUNTIME_FRAME_TYPE_OFFSET]) ==
                   RETRACE_RUNTIME_MESSAGE_HANDSHAKE &&
               load_u16_le(&frame[RETRACE_RUNTIME_FRAME_FLAGS_OFFSET]) == 0U &&
               load_u32_le(&frame[RETRACE_RUNTIME_FRAME_PAYLOAD_SIZE_OFFSET]) == 0U,
           "the handshake has the expected type, flags, and empty payload");
  }
  expect(wait_for_success(child), "the handshaking target continues normally");
}

static void run_unavailable_channel_case(const char* const value,
                                         const char* const message) {
  const pid_t child = fork();
  expect(child >= 0, "an unavailable-channel child can be created");
  if (child < 0) {
    return;
  }
  if (child == 0) {
    const int environment_result = value == NULL
                                       ? unsetenv(RETRACE_RUNTIME_EVENT_FD_ENV)
                                       : setenv(RETRACE_RUNTIME_EVENT_FD_ENV, value, 1);
    if (environment_result < 0 || load_runtime() < 0) {
      _exit(EXIT_FAILURE);
    }
    _exit(EXIT_SUCCESS);
  }
  expect(wait_for_success(child), message);
}

static void test_closed_channel_is_nonfatal(void) {
  int channel[2] = {-1, -1};
  expect(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, channel) == 0,
         "a channel is available for the closed-peer test");
  if (channel[0] < 0 || channel[1] < 0) {
    return;
  }
  close(channel[0]);

  const pid_t child = fork();
  expect(child >= 0, "a closed-channel child can be created");
  if (child < 0) {
    close(channel[1]);
    return;
  }
  if (child == 0) {
    if (set_descriptor_environment(channel[1]) < 0 || load_runtime() < 0) {
      _exit(EXIT_FAILURE);
    }
    close(channel[1]);
    _exit(EXIT_SUCCESS);
  }

  close(channel[1]);
  expect(wait_for_success(child),
         "a closed runtime channel does not terminate or fail the target");
}

static void test_full_channel_is_nonfatal(void) {
  int channel[2] = {-1, -1};
  expect(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, channel) == 0,
         "a channel is available for the full-buffer test");
  if (channel[0] < 0 || channel[1] < 0) {
    return;
  }

  unsigned char packet[4096] = {0U};
  while (1) {
    const ssize_t result =
        send(channel[1], packet, sizeof(packet), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (result > 0) {
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    expect(result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
           "the test fills the runtime channel without another send error");
    break;
  }

  const pid_t child = fork();
  expect(child >= 0, "a full-channel child can be created");
  if (child < 0) {
    close(channel[0]);
    close(channel[1]);
    return;
  }
  if (child == 0) {
    alarm(2U);
    if (set_descriptor_environment(channel[1]) < 0 || load_runtime() < 0) {
      _exit(EXIT_FAILURE);
    }
    close(channel[0]);
    close(channel[1]);
    _exit(EXIT_SUCCESS);
  }

  close(channel[0]);
  close(channel[1]);
  expect(wait_for_success(child), "a full runtime channel does not block the target");
}

int main(void) {
  test_handshake_frame();
  run_unavailable_channel_case(NULL,
                               "an absent runtime channel does not affect the target");
  run_unavailable_channel_case(
      "", "an empty runtime channel setting does not affect the target");
  run_unavailable_channel_case(
      "0", "a standard descriptor is not treated as the runtime channel");
  run_unavailable_channel_case(
      "not-a-descriptor",
      "an invalid runtime channel setting does not affect the target");
  run_unavailable_channel_case(
      "999999999999999999999999",
      "an overflowing runtime channel setting does not affect the target");
  test_closed_channel_is_nonfatal();
  test_full_channel_is_nonfatal();
  return failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
