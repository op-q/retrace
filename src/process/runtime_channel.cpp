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

RuntimeReceiveResult RuntimeChannel::receive() const {
  // MSG_TRUNC asks Linux to report a packet's real size even if the fixed buffer
  // is too small, allowing oversized packets to be rejected deterministically.
  constexpr auto maximum_frame_size =
      static_cast<std::size_t>(RETRACE_RUNTIME_FRAME_HEADER_SIZE) +
      static_cast<std::size_t>(RETRACE_RUNTIME_FRAME_MAX_PAYLOAD_SIZE);
  std::array<unsigned char, maximum_frame_size> frame{};
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
  if (type != RETRACE_RUNTIME_MESSAGE_HANDSHAKE || payload_size != 0U) {
    return {.status = RuntimeReceiveStatus::error,
            .error = std::make_error_code(std::errc::protocol_error)};
  }

  return {.status = RuntimeReceiveStatus::handshake, .error = {}};
}

}  // namespace retrace::process
