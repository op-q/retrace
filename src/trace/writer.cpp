// Append-only trace encoder. It writes explicit little-endian fields and closes
// permanently after the first write error so a corrupt middle cannot be hidden.

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "retrace/trace.hpp"

namespace retrace::trace {
namespace {

constexpr std::array<std::byte, 8> trace_magic{
    std::byte{'R'}, std::byte{'E'}, std::byte{'T'}, std::byte{'R'},
    std::byte{'A'}, std::byte{'C'}, std::byte{'E'}, std::byte{0}};
constexpr std::size_t trace_header_prefix_size = 16U;
constexpr std::size_t event_header_size = 28U;
constexpr std::uint32_t event_header_after_length_size = 24U;

[[nodiscard]] std::error_code system_error(const int error_number) {
  return {error_number, std::generic_category()};
}

[[nodiscard]] std::error_code move_above_standard_descriptors(int& descriptor) {
  if (descriptor > STDERR_FILENO) {
    return {};
  }

  int replacement = -1;
  do {
    replacement = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  } while (replacement < 0 && errno == EINTR);
  if (replacement < 0) {
    return system_error(errno);
  }

  ::close(descriptor);
  descriptor = replacement;
  return {};
}

void encode_u16(std::byte* destination, const std::uint16_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    destination[index] =
        static_cast<std::byte>((value >> (index * 8U)) & std::uint16_t{0xffU});
  }
}

void encode_u32(std::byte* destination, const std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    destination[index] =
        static_cast<std::byte>((value >> (index * 8U)) & std::uint32_t{0xffU});
  }
}

void encode_u64(std::byte* destination, const std::uint64_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    destination[index] =
        static_cast<std::byte>((value >> (index * 8U)) & std::uint64_t{0xffU});
  }
}

void append_u32(std::vector<std::byte>& destination, const std::uint32_t value) {
  const auto offset = destination.size();
  destination.resize(offset + sizeof(value));
  encode_u32(destination.data() + offset, value);
}

void append_u64(std::vector<std::byte>& destination, const std::uint64_t value) {
  const auto offset = destination.size();
  destination.resize(offset + sizeof(value));
  encode_u64(destination.data() + offset, value);
}

[[nodiscard]] std::error_code append_string(std::vector<std::byte>& destination,
                                            const std::string_view value) {
  constexpr auto length_size = sizeof(std::uint32_t);
  if (value.size() > std::numeric_limits<std::uint32_t>::max() ||
      destination.size() > maximum_header_payload_size ||
      value.size() > maximum_header_payload_size - destination.size() ||
      length_size > maximum_header_payload_size - destination.size() - value.size()) {
    return std::make_error_code(std::errc::message_size);
  }

  append_u32(destination, static_cast<std::uint32_t>(value.size()));
  if (!value.empty()) {
    const auto* begin = reinterpret_cast<const std::byte*>(value.data());
    destination.insert(destination.end(), begin, begin + value.size());
  }
  return {};
}

[[nodiscard]] std::error_code clock_nanoseconds(const clockid_t clock,
                                                std::uint64_t& result) {
  timespec value{};
  if (::clock_gettime(clock, &value) < 0) {
    return system_error(errno);
  }
  if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L) {
    return std::make_error_code(std::errc::value_too_large);
  }

  constexpr std::uint64_t nanoseconds_per_second = 1000000000U;
  const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
  const auto nanoseconds = static_cast<std::uint64_t>(value.tv_nsec);
  if (seconds > (std::numeric_limits<std::uint64_t>::max() - nanoseconds) /
                    nanoseconds_per_second) {
    return std::make_error_code(std::errc::value_too_large);
  }

  result = seconds * nanoseconds_per_second + nanoseconds;
  return {};
}

[[nodiscard]] std::error_code write_all(const int descriptor, const void* data,
                                        const std::size_t size) {
  // write(2) may complete partially or be interrupted. This loop advances only
  // by bytes the kernel confirmed, preserving exact frame boundaries.
  const auto* bytes = static_cast<const std::byte*>(data);
  std::size_t bytes_written = 0U;
  while (bytes_written < size) {
    const auto write_result =
        ::write(descriptor, bytes + bytes_written, size - bytes_written);
    if (write_result > 0) {
      bytes_written += static_cast<std::size_t>(write_result);
      continue;
    }
    if (write_result < 0 && errno == EINTR) {
      continue;
    }
    return write_result == 0 ? std::make_error_code(std::errc::io_error)
                             : system_error(errno);
  }
  return {};
}

[[nodiscard]] std::error_code build_header(const Metadata& metadata,
                                           const std::uint64_t realtime_nanoseconds,
                                           const std::uint64_t monotonic_nanoseconds,
                                           std::vector<std::byte>& result) {
  if (metadata.arguments.empty() ||
      metadata.arguments.size() > maximum_argument_count) {
    return std::make_error_code(std::errc::message_size);
  }

  std::vector<std::byte> payload;
  payload.reserve(256U);
  append_u64(payload, realtime_nanoseconds);
  append_u64(payload, monotonic_nanoseconds);

  for (const auto field : {metadata.retrace_version, metadata.operating_system,
                           metadata.architecture, metadata.working_directory}) {
    if (const auto error = append_string(payload, field)) {
      return error;
    }
  }

  if (payload.size() > maximum_header_payload_size - sizeof(std::uint32_t)) {
    return std::make_error_code(std::errc::message_size);
  }
  append_u32(payload, static_cast<std::uint32_t>(metadata.arguments.size()));
  for (const auto argument : metadata.arguments) {
    if (const auto error = append_string(payload, argument)) {
      return error;
    }
  }

  result.resize(trace_header_prefix_size + payload.size());
  std::copy(trace_magic.begin(), trace_magic.end(), result.begin());
  encode_u16(result.data() + 8U, format_major_version);
  encode_u16(result.data() + 10U, format_minor_version);
  encode_u32(result.data() + 12U, static_cast<std::uint32_t>(payload.size()));
  std::copy(payload.begin(), payload.end(), result.begin() + trace_header_prefix_size);
  return {};
}

}  // namespace

Writer::~Writer() { close(); }

Writer::Writer(Writer&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)),
      monotonic_base_nanoseconds_(
          std::exchange(other.monotonic_base_nanoseconds_, 0U)) {}

Writer& Writer::operator=(Writer&& other) noexcept {
  if (this != &other) {
    close();
    descriptor_ = std::exchange(other.descriptor_, -1);
    monotonic_base_nanoseconds_ = std::exchange(other.monotonic_base_nanoseconds_, 0U);
  }
  return *this;
}

CreationResult Writer::create(const std::filesystem::path& path,
                              const Metadata& metadata, Writer& result) {
  // Metadata is fully encoded and checked before opening the destination. Once
  // created, the file is exclusive, user-only, and never overwritten.
  if (path.empty() || path.native().find('\0') != std::string::npos) {
    return {.error = std::make_error_code(std::errc::invalid_argument)};
  }

  std::uint64_t realtime_nanoseconds = 0U;
  if (const auto error = clock_nanoseconds(CLOCK_REALTIME, realtime_nanoseconds)) {
    return {.error = error};
  }
  std::uint64_t monotonic_nanoseconds = 0U;
  if (const auto error = clock_nanoseconds(CLOCK_MONOTONIC, monotonic_nanoseconds)) {
    return {.error = error};
  }

  std::vector<std::byte> header;
  if (const auto error =
          build_header(metadata, realtime_nanoseconds, monotonic_nanoseconds, header)) {
    return {.error = error};
  }

  int descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
             S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    return {.error = system_error(errno)};
  }
  if (const auto error = move_above_standard_descriptors(descriptor)) {
    ::close(descriptor);
    return {.error = error, .file_created = true};
  }

  Writer candidate;
  candidate.descriptor_ = descriptor;
  candidate.monotonic_base_nanoseconds_ = monotonic_nanoseconds;
  if (const auto error =
          write_all(candidate.descriptor_, header.data(), header.size())) {
    return {.error = error, .file_created = true};
  }

  result = std::move(candidate);
  return {.error = {}, .file_created = true};
}

std::error_code Writer::write_event(const EventType type,
                                    const std::uint32_t process_id,
                                    const std::string_view payload) {
  // Event schemas are checked here as well as by the reader. A writer bug should
  // not be able to create bytes that its matching reader rejects.
  if (!is_open()) {
    return std::make_error_code(std::errc::bad_file_descriptor);
  }
  if (payload.size() > maximum_event_payload_size) {
    return std::make_error_code(std::errc::message_size);
  }

  switch (type) {
    case EventType::process_start:
    case EventType::process_exec:
    case EventType::runtime_handshake:
      if (!payload.empty()) {
        return std::make_error_code(std::errc::invalid_argument);
      }
      break;
    case EventType::standard_output:
    case EventType::standard_error:
      break;
    case EventType::process_exit:
    case EventType::process_signal:
    case EventType::process_launch_failure:
    case EventType::signal_receive:
      if (payload.size() != sizeof(std::uint32_t)) {
        return std::make_error_code(std::errc::invalid_argument);
      }
      break;
  }

  std::uint64_t now_nanoseconds = 0U;
  if (const auto error = clock_nanoseconds(CLOCK_MONOTONIC, now_nanoseconds)) {
    return error;
  }
  const auto offset_nanoseconds = now_nanoseconds >= monotonic_base_nanoseconds_
                                      ? now_nanoseconds - monotonic_base_nanoseconds_
                                      : 0U;

  std::array<std::byte, event_header_size> header{};
  const auto payload_size = static_cast<std::uint32_t>(payload.size());
  encode_u32(header.data(), event_header_after_length_size + payload_size);
  encode_u16(header.data() + 4U, static_cast<std::uint16_t>(type));
  encode_u16(header.data() + 6U, 0U);
  encode_u64(header.data() + 8U, offset_nanoseconds);
  encode_u32(header.data() + 16U, process_id);
  encode_u32(header.data() + 20U, 0U);
  encode_u32(header.data() + 24U, payload_size);

  if (const auto error = write_all(descriptor_, header.data(), header.size())) {
    close();
    return error;
  }
  if (!payload.empty()) {
    if (const auto error = write_all(descriptor_, payload.data(), payload.size())) {
      close();
      return error;
    }
  }
  return {};
}

// The two u32 values occupy distinct fields selected by the strongly typed event.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::error_code Writer::write_value_event(const EventType type,
                                          const std::uint32_t process_id,
                                          const std::uint32_t value) {
  if (type != EventType::process_exit && type != EventType::process_signal &&
      type != EventType::process_launch_failure && type != EventType::signal_receive) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  std::array<std::byte, sizeof(value)> payload{};
  encode_u32(payload.data(), value);
  return write_event(type, process_id,
                     {reinterpret_cast<const char*>(payload.data()), payload.size()});
}
// NOLINTEND(bugprone-easily-swappable-parameters)

std::error_code Writer::finish() noexcept {
  if (descriptor_ < 0) {
    return {};
  }

  const int descriptor = std::exchange(descriptor_, -1);
  monotonic_base_nanoseconds_ = 0U;
  // On Linux the descriptor is released even when close reports EINTR. Retrying
  // could close a descriptor that another thread has since reused.
  if (::close(descriptor) < 0) {
    return system_error(errno);
  }
  return {};
}

void Writer::close() noexcept { [[maybe_unused]] const auto finish_error = finish(); }

}  // namespace retrace::trace
