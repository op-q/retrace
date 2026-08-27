// Injected C17 runtime. Its constructor performs one fail-open handshake and
// installs libc interposition for the selected file operations. Every hook is a
// guest inside another process: it preserves errno, avoids allocation, bounds
// its payloads, and falls back to the raw syscall rather than failing a call it
// could not resolve.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "retrace/runtime_protocol.h"

// Interposition requires the hook symbols to be visible to the dynamic linker.
// The library is otherwise built with hidden visibility, so each replacement is
// exported deliberately and nothing else in this file becomes part of the ABI.
#define RETRACE_EXPORT __attribute__((visibility("default")))

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

static void store_u64_le(unsigned char* const destination, const uint64_t value) {
  for (unsigned int index = 0U; index < 8U; ++index) {
    destination[index] = (unsigned char)((value >> (index * 8U)) & UINT64_C(0xff));
  }
}

// Runtime state
//
// `event_descriptor` is written once by the constructor before the target can
// create threads. `emission_enabled` gates every send and is cleared when the
// channel becomes unusable, so the hooks degrade to plain pass-through calls.
static int event_descriptor = -1;
static atomic_ullong operation_sequence;
static atomic_int emission_enabled;

// The recursion guard uses the initial-exec TLS model because the general
// dynamic model can allocate on first access, and a hook must not enter malloc
// on a path that may itself have been reached from an allocator.
static __thread int inside_hook __attribute__((tls_model("initial-exec")));

typedef int (*open_function)(const char*, int, ...);
typedef int (*openat_function)(int, const char*, int, ...);
typedef int (*close_function)(int);

static open_function real_open;
static open_function real_open64;
static openat_function real_openat;
static openat_function real_openat64;
static close_function real_close;

// ISO C has no conversion between object and function pointers, which dlsym(3)
// nonetheless requires. A union performs the POSIX-sanctioned reinterpretation
// without tripping the project's pedantic warning set.
static open_function resolve_open(const char* const name) {
  union {
    void* object;
    open_function function;
  } converter;
  converter.object = dlsym(RTLD_NEXT, name);
  return converter.function;
}

static openat_function resolve_openat(const char* const name) {
  union {
    void* object;
    openat_function function;
  } converter;
  converter.object = dlsym(RTLD_NEXT, name);
  return converter.function;
}

static close_function resolve_close(const char* const name) {
  union {
    void* object;
    close_function function;
  } converter;
  converter.object = dlsym(RTLD_NEXT, name);
  return converter.function;
}

// A hook can run before this library's constructor when an earlier preloaded
// object opens a file from its own constructor. Resolution is therefore also
// attempted lazily. Two threads racing here write the same value, so the
// unsynchronized store is benign.
static open_function open_target(void) {
  if (real_open == NULL) {
    real_open = resolve_open("open");
  }
  return real_open;
}

static open_function open64_target(void) {
  if (real_open64 == NULL) {
    real_open64 = resolve_open("open64");
  }
  return real_open64;
}

static openat_function openat_target(void) {
  if (real_openat == NULL) {
    real_openat = resolve_openat("openat");
  }
  return real_openat;
}

static openat_function openat64_target(void) {
  if (real_openat64 == NULL) {
    real_openat64 = resolve_openat("openat64");
  }
  return real_openat64;
}

static close_function close_target(void) {
  if (real_close == NULL) {
    real_close = resolve_close("close");
  }
  return real_close;
}

// Resolution failure must not turn into a failed target operation. open(2) is
// openat(2) against the current directory, so one fallback serves both.
static int fallback_openat(const int directory, const char* const path, const int flags,
                           const mode_t mode) {
  return (int)syscall(SYS_openat, directory, path, flags, (unsigned int)mode);
}

static int fallback_close(const int descriptor) {
  return (int)syscall(SYS_close, descriptor);
}

static uint64_t monotonic_nanoseconds(void) {
  // CLOCK_MONOTONIC matches the trace header's base, keeping runtime timestamps
  // comparable with supervisor-observed offsets.
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0U;
  }
  return ((uint64_t)now.tv_sec * UINT64_C(1000000000)) + (uint64_t)now.tv_nsec;
}

static uint32_t current_thread_identifier(void) {
  return (uint32_t)syscall(SYS_gettid);
}

static void emit_file_operation(const uint16_t operation_id, const int64_t result,
                                const int error_number, const int descriptor,
                                const int directory, const uint32_t open_flags,
                                const uint32_t mode, const char* const path,
                                const uint64_t started, const uint64_t completed) {
  if (inside_hook != 0 || atomic_load(&emission_enabled) == 0) {
    return;
  }
  inside_hook = 1;

  const int failed = result < 0;

  // Reading the path is only safe once the real call has accepted the pointer.
  // A target that passed an unmapped address receives EFAULT rather than a
  // crash, and this runtime must not convert that into a fault of its own.
  uint32_t path_size = 0U;
  uint16_t operation_flags = 0U;
  if (path != NULL && !(failed && error_number == EFAULT)) {
    const size_t limit = (size_t)RETRACE_RUNTIME_FILE_MAX_PATH_SIZE;
    const size_t measured = strnlen(path, limit + 1U);
    if (measured > limit) {
      path_size = RETRACE_RUNTIME_FILE_MAX_PATH_SIZE;
      operation_flags |= RETRACE_RUNTIME_OPERATION_FLAG_PATH_TRUNCATED;
    } else {
      path_size = (uint32_t)measured;
    }
  }

  // The frame lives on the target thread's stack because a shared static buffer
  // would need a lock and a thread-local one would enlarge every thread's TLS.
  // The bounded path keeps this allocation just over four kilobytes.
  unsigned char frame[RETRACE_RUNTIME_FRAME_HEADER_SIZE +
                      RETRACE_RUNTIME_FILE_HEADER_SIZE +
                      RETRACE_RUNTIME_FILE_MAX_PATH_SIZE];
  const uint32_t payload_size = RETRACE_RUNTIME_FILE_HEADER_SIZE + path_size;

  frame[RETRACE_RUNTIME_FRAME_MAGIC_OFFSET + 0U] = RETRACE_RUNTIME_PROTOCOL_MAGIC_0;
  frame[RETRACE_RUNTIME_FRAME_MAGIC_OFFSET + 1U] = RETRACE_RUNTIME_PROTOCOL_MAGIC_1;
  frame[RETRACE_RUNTIME_FRAME_MAGIC_OFFSET + 2U] = RETRACE_RUNTIME_PROTOCOL_MAGIC_2;
  frame[RETRACE_RUNTIME_FRAME_MAGIC_OFFSET + 3U] = RETRACE_RUNTIME_PROTOCOL_MAGIC_3;
  store_u16_le(&frame[RETRACE_RUNTIME_FRAME_MAJOR_OFFSET],
               RETRACE_RUNTIME_PROTOCOL_MAJOR);
  store_u16_le(&frame[RETRACE_RUNTIME_FRAME_MINOR_OFFSET],
               RETRACE_RUNTIME_PROTOCOL_MINOR);
  store_u16_le(&frame[RETRACE_RUNTIME_FRAME_TYPE_OFFSET],
               (uint16_t)RETRACE_RUNTIME_MESSAGE_OPERATION);
  store_u16_le(&frame[RETRACE_RUNTIME_FRAME_FLAGS_OFFSET], UINT16_C(0));
  store_u32_le(&frame[RETRACE_RUNTIME_FRAME_PAYLOAD_SIZE_OFFSET], payload_size);

  unsigned char* const payload = &frame[RETRACE_RUNTIME_FRAME_HEADER_SIZE];

  // The sequence number counts attempted reports. Consuming one even when the
  // send below is dropped is what lets the supervisor detect the loss as a gap
  // instead of silently under-reporting the target's behavior.
  const uint64_t sequence = (uint64_t)atomic_fetch_add(&operation_sequence, 1ULL);

  store_u64_le(&payload[RETRACE_RUNTIME_OPERATION_SEQUENCE_OFFSET], sequence);
  store_u64_le(&payload[RETRACE_RUNTIME_OPERATION_MONOTONIC_OFFSET], completed);
  store_u64_le(&payload[RETRACE_RUNTIME_OPERATION_DURATION_OFFSET],
               completed >= started ? completed - started : UINT64_C(0));
  store_u32_le(&payload[RETRACE_RUNTIME_OPERATION_THREAD_OFFSET],
               current_thread_identifier());
  store_u16_le(&payload[RETRACE_RUNTIME_OPERATION_ID_OFFSET], operation_id);
  store_u16_le(&payload[RETRACE_RUNTIME_OPERATION_FLAGS_OFFSET], operation_flags);

  store_u64_le(&payload[RETRACE_RUNTIME_FILE_RESULT_OFFSET], (uint64_t)result);
  store_u32_le(&payload[RETRACE_RUNTIME_FILE_ERRNO_OFFSET],
               failed ? (uint32_t)error_number : UINT32_C(0));
  store_u32_le(&payload[RETRACE_RUNTIME_FILE_DESCRIPTOR_OFFSET], (uint32_t)descriptor);
  store_u32_le(&payload[RETRACE_RUNTIME_FILE_DIRECTORY_OFFSET], (uint32_t)directory);
  store_u32_le(&payload[RETRACE_RUNTIME_FILE_OPEN_FLAGS_OFFSET], open_flags);
  store_u32_le(&payload[RETRACE_RUNTIME_FILE_MODE_OFFSET], mode);
  store_u32_le(&payload[RETRACE_RUNTIME_FILE_PATH_SIZE_OFFSET], path_size);
  if (path_size > 0U) {
    memcpy(&payload[RETRACE_RUNTIME_FILE_HEADER_SIZE], path, (size_t)path_size);
  }

  const size_t frame_size =
      (size_t)RETRACE_RUNTIME_FRAME_HEADER_SIZE + (size_t)payload_size;
  ssize_t sent = -1;
  do {
    sent = send(event_descriptor, frame, frame_size, MSG_NOSIGNAL | MSG_DONTWAIT);
  } while (sent < 0 && errno == EINTR);

  inside_hook = 0;
}

static int record_open(const uint16_t operation_id, const int directory,
                       const char* const path, const int flags, const mode_t mode,
                       const int result, const int saved_errno, const uint64_t started,
                       const uint64_t completed) {
  emit_file_operation(operation_id, (int64_t)result, saved_errno, result, directory,
                      (uint32_t)flags, (uint32_t)mode, path, started, completed);
  errno = saved_errno;
  return result;
}

// open(2) and openat(2) take the mode argument only for the flags that can
// create a file. Reading it unconditionally would consume an argument the
// caller never passed.
static mode_t creation_mode(const int flags, va_list arguments) {
  if ((flags & (O_CREAT | O_TMPFILE)) == 0) {
    return 0;
  }
  return (mode_t)va_arg(arguments, unsigned int);
}

RETRACE_EXPORT int open(const char* const path, const int flags, ...) {
  va_list arguments;
  va_start(arguments, flags);
  const mode_t mode = creation_mode(flags, arguments);
  va_end(arguments);

  const open_function target = open_target();
  const uint64_t started = monotonic_nanoseconds();
  const int result = target != NULL ? target(path, flags, mode)
                                    : fallback_openat(AT_FDCWD, path, flags, mode);
  const int saved_errno = errno;
  return record_open(RETRACE_RUNTIME_OPERATION_OPEN, AT_FDCWD, path, flags, mode,
                     result, saved_errno, started, monotonic_nanoseconds());
}

RETRACE_EXPORT int open64(const char* const path, const int flags, ...) {
  va_list arguments;
  va_start(arguments, flags);
  const mode_t mode = creation_mode(flags, arguments);
  va_end(arguments);

  const open_function target = open64_target();
  const uint64_t started = monotonic_nanoseconds();
  const int result = target != NULL ? target(path, flags, mode)
                                    : fallback_openat(AT_FDCWD, path, flags, mode);
  const int saved_errno = errno;
  return record_open(RETRACE_RUNTIME_OPERATION_OPEN, AT_FDCWD, path, flags, mode,
                     result, saved_errno, started, monotonic_nanoseconds());
}

RETRACE_EXPORT int openat(const int directory, const char* const path, const int flags,
                          ...) {
  va_list arguments;
  va_start(arguments, flags);
  const mode_t mode = creation_mode(flags, arguments);
  va_end(arguments);

  const openat_function target = openat_target();
  const uint64_t started = monotonic_nanoseconds();
  const int result = target != NULL ? target(directory, path, flags, mode)
                                    : fallback_openat(directory, path, flags, mode);
  const int saved_errno = errno;
  return record_open(RETRACE_RUNTIME_OPERATION_OPENAT, directory, path, flags, mode,
                     result, saved_errno, started, monotonic_nanoseconds());
}

RETRACE_EXPORT int openat64(const int directory, const char* const path,
                            const int flags, ...) {
  va_list arguments;
  va_start(arguments, flags);
  const mode_t mode = creation_mode(flags, arguments);
  va_end(arguments);

  const openat_function target = openat64_target();
  const uint64_t started = monotonic_nanoseconds();
  const int result = target != NULL ? target(directory, path, flags, mode)
                                    : fallback_openat(directory, path, flags, mode);
  const int saved_errno = errno;
  return record_open(RETRACE_RUNTIME_OPERATION_OPENAT, directory, path, flags, mode,
                     result, saved_errno, started, monotonic_nanoseconds());
}

RETRACE_EXPORT int close(const int descriptor) {
  const close_function target = close_target();

  // Losing the channel to the target would be unsafe rather than merely lossy.
  // The kernel reuses the lowest free descriptor number, so a later target open
  // could receive this one and then receive protocol frames into its own file.
  const int closing_channel = event_descriptor >= 0 && descriptor == event_descriptor;

  const uint64_t started = monotonic_nanoseconds();
  const int result = target != NULL ? target(descriptor) : fallback_close(descriptor);
  const int saved_errno = errno;
  const uint64_t completed = monotonic_nanoseconds();

  if (closing_channel && result == 0) {
    atomic_store(&emission_enabled, 0);
  } else {
    emit_file_operation(RETRACE_RUNTIME_OPERATION_CLOSE, (int64_t)result, saved_errno,
                        descriptor, 0, UINT32_C(0), UINT32_C(0), NULL, started,
                        completed);
  }

  errno = saved_errno;
  return result;
}

static void make_descriptor_close_on_exec(const int descriptor) {
  // The supervisor clears this flag for the first target exec. Restoring it in
  // the loaded image prevents an exec'd descendant from emitting a second
  // session handshake through an inherited channel.
  int flags = -1;
  do {
    flags = fcntl(descriptor, F_GETFD);
  } while (flags < 0 && errno == EINTR);
  if (flags < 0) {
    return;
  }

  int result = -1;
  do {
    result = fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
  } while (result < 0 && errno == EINTR);
}

static int parse_event_descriptor(const char* const text) {
  // Manual decimal parsing avoids locale, allocation, and ambiguous partial
  // conversions during dynamic-loader startup.
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

static void emit_handshake(const int descriptor) {
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

// A child that forks without exec inherits this library's state and the open
// channel. Emitting from it would attribute another process's operations to the
// recorded target, so the child stops reporting instead.
static void disable_emission_after_fork(void) { atomic_store(&emission_enabled, 0); }

static void retrace_runtime_start(void) __attribute__((constructor));

static void retrace_runtime_start(void) {
  // Instrumentation failure must not block startup, raise SIGPIPE, or alter the
  // caller-visible errno value.
  const int saved_errno = errno;

  // Resolving here keeps the common path free of dlsym work, while the lazy
  // resolution in each hook still covers calls that precede this constructor.
  real_open = resolve_open("open");
  real_open64 = resolve_open("open64");
  real_openat = resolve_openat("openat");
  real_openat64 = resolve_openat("openat64");
  real_close = resolve_close("close");

  const int descriptor = parse_event_descriptor(getenv(RETRACE_RUNTIME_EVENT_FD_ENV));
  if (descriptor >= 0) {
    make_descriptor_close_on_exec(descriptor);
    event_descriptor = descriptor;
    (void)pthread_atfork(NULL, NULL, disable_emission_after_fork);
    atomic_store(&emission_enabled, 1);
    emit_handshake(descriptor);
  }

  errno = saved_errno;
}
