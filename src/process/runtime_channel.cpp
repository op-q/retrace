// Creates the supervisor/runtime SOCK_SEQPACKET pair and validates complete,
// bounded protocol frames without trusting target-provided sizes or fields.

#include "runtime_channel.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <utility>

#include "retrace/runtime_protocol.h"

namespace retrace::process {
namespace {

[[nodiscard]] std::error_code system_error(const int error_number) {
  return {error_number, std::generic_category()};
}

[[nodiscard]] std::error_code move_above_standard_descriptors(UniqueFd& descriptor) {
  // socketpair(2) may return 0, 1, or 2 when the caller closed a standard
  // stream. Internal descriptors must not collide with later stdio redirection.
  if (descriptor.get() > STDERR_FILENO) {
    return {};
  }

  int result = -1;
  do {
    result = ::fcntl(descriptor.get(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    return system_error(errno);
  }

  descriptor.reset(result);
  return {};
}

[[nodiscard]] std::uint16_t load_u16_le(const unsigned char* const source) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(source[0]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(source[1]) << 8U));
}

[[nodiscard]] std::uint32_t load_u32_le(const unsigned char* const source) {
  return static_cast<std::uint32_t>(source[0]) |
         (static_cast<std::uint32_t>(source[1]) << 8U) |
         (static_cast<std::uint32_t>(source[2]) << 16U) |
         (static_cast<std::uint32_t>(source[3]) << 24U);
}

[[nodiscard]] std::uint64_t load_u64_le(const unsigned char* const source) {
  std::uint64_t result = 0U;
  for (unsigned int index = 0U; index < 8U; ++index) {
    result |= static_cast<std::uint64_t>(source[index]) << (index * 8U);
  }
  return result;
}

[[nodiscard]] RuntimeReceiveResult protocol_error() {
  return {.status = RuntimeReceiveStatus::error,
          .error = std::make_error_code(std::errc::protocol_error)};
}

// Decodes one operation payload. Every field is validated against the declared
// payload size before use: the target controls these bytes, so a length it
// supplies must never be trusted to address memory.
[[nodiscard]] RuntimeReceiveResult decode_operation(const unsigned char* const payload,
                                                    const std::uint32_t payload_size,
                                                    RuntimeOperation& operation) {
  if (payload_size < RETRACE_RUNTIME_OPERATION_PREFIX_SIZE) {
    return protocol_error();
  }

  const auto flags = load_u16_le(&payload[RETRACE_RUNTIME_OPERATION_FLAGS_OFFSET]);
  if ((flags & ~RETRACE_RUNTIME_OPERATION_FLAGS_DEFINED) != 0U) {
    return protocol_error();
  }

  // An unrecognized operation identifier selects an unknown tail layout, so the
  // frame cannot be decoded safely and is rejected rather than guessed at.
  RuntimeOperationKind kind{};
  switch (load_u16_le(&payload[RETRACE_RUNTIME_OPERATION_ID_OFFSET])) {
    case RETRACE_RUNTIME_OPERATION_OPEN:
      kind = RuntimeOperationKind::file_open;
      break;
    case RETRACE_RUNTIME_OPERATION_OPENAT:
      kind = RuntimeOperationKind::file_openat;
      break;
    case RETRACE_RUNTIME_OPERATION_CLOSE:
      kind = RuntimeOperationKind::file_close;
      break;
    default:
      return protocol_error();
  }

  // Every operation defined in version 1.0 carries the file tail.
  if (payload_size < RETRACE_RUNTIME_FILE_HEADER_SIZE) {
    return protocol_error();
  }
  const auto path_size = load_u32_le(&payload[RETRACE_RUNTIME_FILE_PATH_SIZE_OFFSET]);
  if (path_size > RETRACE_RUNTIME_FILE_MAX_PATH_SIZE ||
      path_size != payload_size - RETRACE_RUNTIME_FILE_HEADER_SIZE) {
    return protocol_error();
  }

  operation.sequence = load_u64_le(&payload[RETRACE_RUNTIME_OPERATION_SEQUENCE_OFFSET]);
  operation.monotonic_nanoseconds =
      load_u64_le(&payload[RETRACE_RUNTIME_OPERATION_MONOTONIC_OFFSET]);
  operation.duration_nanoseconds =
      load_u64_le(&payload[RETRACE_RUNTIME_OPERATION_DURATION_OFFSET]);
  operation.thread_id = load_u32_le(&payload[RETRACE_RUNTIME_OPERATION_THREAD_OFFSET]);
  operation.kind = kind;
  operation.path_truncated =
      (flags & RETRACE_RUNTIME_OPERATION_FLAG_PATH_TRUNCATED) != 0U;
  operation.result = static_cast<std::int64_t>(
      load_u64_le(&payload[RETRACE_RUNTIME_FILE_RESULT_OFFSET]));
  operation.error_number = load_u32_le(&payload[RETRACE_RUNTIME_FILE_ERRNO_OFFSET]);
  operation.descriptor = static_cast<std::int32_t>(
      load_u32_le(&payload[RETRACE_RUNTIME_FILE_DESCRIPTOR_OFFSET]));
  operation.directory = static_cast<std::int32_t>(
      load_u32_le(&payload[RETRACE_RUNTIME_FILE_DIRECTORY_OFFSET]));
  operation.open_flags = load_u32_le(&payload[RETRACE_RUNTIME_FILE_OPEN_FLAGS_OFFSET]);
  operation.mode = load_u32_le(&payload[RETRACE_RUNTIME_FILE_MODE_OFFSET]);
  operation.path.assign(
      reinterpret_cast<const char*>(&payload[RETRACE_RUNTIME_FILE_HEADER_SIZE]),
      path_size);

  return {.status = RuntimeReceiveStatus::operation, .error = {}};
}

}  // namespace

std::error_code RuntimeChannel::create(RuntimeChannel& result) {
  // SOCK_SEQPACKET preserves one-send/one-receive message boundaries.
  // CLOEXEC is the safe default; only the target end is cleared in the child.
  std::array<int, 2> descriptors{};
  if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
                   descriptors.data()) < 0) {
    return system_error(errno);
  }

  RuntimeChannel candidate;
  candidate.supervisor_end_.reset(descriptors[0]);
  candidate.target_end_.reset(descriptors[1]);

  if (const auto error = move_above_standard_descriptors(candidate.supervisor_end_)) {
    return error;
  }
  if (const auto error = move_above_standard_descriptors(candidate.target_end_)) {
    return error;
  }

  if (::shutdown(candidate.supervisor_end_.get(), SHUT_WR) < 0) {
    return system_error(errno);
  }
  if (::shutdown(candidate.target_end_.get(), SHUT_RD) < 0) {
    return system_error(errno);
  }

  result = std::move(candidate);
  return {};
}

int RuntimeChannel::make_target_descriptor_inheritable() const noexcept {
  // Called after fork in the child. fcntl is used instead of environment
  // mutation or allocation in this sensitive pre-exec window.
  int flags = -1;
  do {
    flags = ::fcntl(target_end_.get(), F_GETFD);
  } while (flags < 0 && errno == EINTR);
  if (flags < 0) {
    return errno;
  }

  int result = -1;
  do {
    result = ::fcntl(target_end_.get(), F_SETFD, flags & ~FD_CLOEXEC);
  } while (result < 0 && errno == EINTR);
  return result < 0 ? errno : 0;
}

RuntimeReceiveResult RuntimeChannel::receive(RuntimeOperation& operation) const {
  // MSG_TRUNC asks Linux to report a packet's real size even if the fixed buffer
  // is too small, allowing oversized packets to be rejected deterministically.
  constexpr auto maximum_frame_size =
      static_cast<std::size_t>(RETRACE_RUNTIME_FRAME_HEADER_SIZE) +
      static_cast<std::size_t>(RETRACE_RUNTIME_FRAME_MAX_PAYLOAD_SIZE);
  // Value-initializing this buffer would re-zero 64 KiB on every packet, which
  // becomes measurable once operation frames arrive at target speed. Static
  // thread-local storage is zeroed once per thread instead, and each packet is
  // read only up to the length recvmsg(2) reports.
  static thread_local std::array<unsigned char, maximum_frame_size> frame;
  iovec frame_buffer{.iov_base = frame.data(), .iov_len = frame.size()};
  msghdr message{};
  message.msg_iov = &frame_buffer;
  message.msg_iovlen = 1U;

  ssize_t received = -1;
  do {
    received = ::recvmsg(supervisor_end_.get(), &message, MSG_DONTWAIT | MSG_TRUNC);
  } while (received < 0 && errno == EINTR);

  if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return {.status = RuntimeReceiveStatus::would_block, .error = {}};
  }
  if (received < 0) {
    return {.status = RuntimeReceiveStatus::error, .error = system_error(errno)};
  }
  if (received == 0) {
    if ((message.msg_flags & MSG_EOR) != 0) {
      return {.status = RuntimeReceiveStatus::error,
              .error = std::make_error_code(std::errc::protocol_error)};
    }
    return {.status = RuntimeReceiveStatus::closed, .error = {}};
  }
  if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
      static_cast<std::size_t>(received) > frame.size()) {
    return {.status = RuntimeReceiveStatus::error,
            .error = std::make_error_code(std::errc::message_size)};
  }
  if (received < static_cast<ssize_t>(RETRACE_RUNTIME_FRAME_HEADER_SIZE)) {
    return {.status = RuntimeReceiveStatus::error,
            .error = std::make_error_code(std::errc::protocol_error)};
  }

  if (frame[RETRACE_RUNTIME_FRAME_MAGIC_OFFSET + 0U] !=
          RETRACE_RUNTIME_PROTOCOL_MAGIC_0 ||
      frame[RETRACE_RUNTIME_FRAME_MAGIC_OFFSET + 1U] !=
          RETRACE_RUNTIME_PROTOCOL_MAGIC_1 ||
      frame[RETRACE_RUNTIME_FRAME_MAGIC_OFFSET + 2U] !=
          RETRACE_RUNTIME_PROTOCOL_MAGIC_2 ||
      frame[RETRACE_RUNTIME_FRAME_MAGIC_OFFSET + 3U] !=
          RETRACE_RUNTIME_PROTOCOL_MAGIC_3) {
    return {.status = RuntimeReceiveStatus::error,
            .error = std::make_error_code(std::errc::protocol_error)};
  }

  const auto major = load_u16_le(&frame[RETRACE_RUNTIME_FRAME_MAJOR_OFFSET]);
  const auto minor = load_u16_le(&frame[RETRACE_RUNTIME_FRAME_MINOR_OFFSET]);
  const auto type = load_u16_le(&frame[RETRACE_RUNTIME_FRAME_TYPE_OFFSET]);
  const auto flags = load_u16_le(&frame[RETRACE_RUNTIME_FRAME_FLAGS_OFFSET]);
  const auto payload_size =
      load_u32_le(&frame[RETRACE_RUNTIME_FRAME_PAYLOAD_SIZE_OFFSET]);

  if (major != RETRACE_RUNTIME_PROTOCOL_MAJOR ||
      minor > RETRACE_RUNTIME_PROTOCOL_MINOR || flags != 0U) {
    return {.status = RuntimeReceiveStatus::error,
            .error = std::make_error_code(std::errc::protocol_error)};
  }
  if (payload_size > RETRACE_RUNTIME_FRAME_MAX_PAYLOAD_SIZE) {
    return {.status = RuntimeReceiveStatus::error,
            .error = std::make_error_code(std::errc::message_size)};
  }
  if (static_cast<std::size_t>(received) !=
      static_cast<std::size_t>(RETRACE_RUNTIME_FRAME_HEADER_SIZE) +
          static_cast<std::size_t>(payload_size)) {
    return {.status = RuntimeReceiveStatus::error,
            .error = std::make_error_code(std::errc::protocol_error)};
  }
  if (type == RETRACE_RUNTIME_MESSAGE_HANDSHAKE) {
    if (payload_size != 0U) {
      return protocol_error();
    }
    return {.status = RuntimeReceiveStatus::handshake, .error = {}};
  }
  if (type == RETRACE_RUNTIME_MESSAGE_OPERATION) {
    return decode_operation(&frame[RETRACE_RUNTIME_FRAME_HEADER_SIZE], payload_size,
                            operation);
  }

  return protocol_error();
}

}  // namespace retrace::process
