#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "retrace/trace.hpp"

namespace retrace::trace {
namespace {

constexpr std::array<std::byte, 8> trace_magic{
    std::byte{'R'}, std::byte{'E'}, std::byte{'T'}, std::byte{'R'},
    std::byte{'A'}, std::byte{'C'}, std::byte{'E'}, std::byte{0}};
constexpr std::size_t trace_header_prefix_size = 16U;
constexpr std::uint32_t event_header_after_length_size = 24U;

class TraceErrorCategory final : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override { return "retrace.trace"; }

  [[nodiscard]] std::string message(const int condition) const override {
    switch (static_cast<TraceErrc>(condition)) {
      case TraceErrc::invalid_magic:
        return "invalid trace magic";
      case TraceErrc::unsupported_version:
        return "unsupported trace version";
      case TraceErrc::header_too_large:
        return "trace header exceeds its size limit";
      case TraceErrc::truncated_header:
        return "trace header is incomplete";
      case TraceErrc::malformed_header:
        return "trace header is malformed";
      case TraceErrc::not_a_regular_file:
        return "trace path is not a regular file";
      case TraceErrc::frame_too_large:
        return "trace event exceeds its size limit";
      case TraceErrc::malformed_frame:
        return "trace event frame is malformed";
      case TraceErrc::invalid_event:
        return "trace event payload is invalid";
      case TraceErrc::non_monotonic_timestamp:
        return "trace event timestamps are not monotonic";
    }
    return "unknown trace error";
  }
};

const TraceErrorCategory trace_error_category;

[[nodiscard]] std::uint16_t decode_u16(const std::byte* bytes) {
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[0])) |
         static_cast<std::uint16_t>(
             static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[1])) << 8U);
}

[[nodiscard]] std::uint32_t decode_u32(const std::byte* bytes) {
  std::uint32_t result = 0U;
  for (std::size_t index = 0; index < sizeof(result); ++index) {
    result |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[index]))
              << (index * 8U);
  }
  return result;
}

[[nodiscard]] std::uint64_t decode_u64(const std::byte* bytes) {
  std::uint64_t result = 0U;
  for (std::size_t index = 0; index < sizeof(result); ++index) {
    result |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index]))
              << (index * 8U);
  }
  return result;
}

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

struct ExactRead {
  std::size_t bytes_read = 0U;
  std::error_code error;
};

[[nodiscard]] ExactRead read_exact(const int descriptor, void* destination,
                                   const std::size_t size) {
  auto* bytes = static_cast<std::byte*>(destination);
  ExactRead result;
  while (result.bytes_read < size) {
    const auto read_result =
        ::read(descriptor, bytes + result.bytes_read, size - result.bytes_read);
    if (read_result > 0) {
      result.bytes_read += static_cast<std::size_t>(read_result);
      continue;
    }
    if (read_result == 0) {
      return result;
    }
    if (errno == EINTR) {
      continue;
    }
    result.error = system_error(errno);
    return result;
  }
  return result;
}

class HeaderCursor final {
 public:
  explicit HeaderCursor(const std::span<const std::byte> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool read_u32(std::uint32_t& result) {
    if (remaining() < sizeof(result)) {
      return false;
    }
    result = decode_u32(bytes_.data() + offset_);
    offset_ += sizeof(result);
    return true;
  }

  [[nodiscard]] bool read_u64(std::uint64_t& result) {
    if (remaining() < sizeof(result)) {
      return false;
    }
    result = decode_u64(bytes_.data() + offset_);
    offset_ += sizeof(result);
    return true;
  }

  [[nodiscard]] bool read_string(std::string& result) {
    std::uint32_t length = 0U;
    if (!read_u32(length) || length > remaining()) {
      return false;
    }
    const auto size = static_cast<std::size_t>(length);
    result.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }

  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - offset_;
  }

 private:
  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0U;
};

[[nodiscard]] bool is_known_event(const std::uint16_t type) {
  switch (static_cast<EventType>(type)) {
    case EventType::process_start:
    case EventType::process_exec:
    case EventType::standard_output:
    case EventType::standard_error:
    case EventType::process_exit:
    case EventType::process_signal:
    case EventType::process_launch_failure:
    case EventType::signal_receive:
      return true;
  }
  return false;
}

[[nodiscard]] bool payload_shape_is_valid(const Event& event) {
  if (!is_known_event(event.type)) {
    return true;
  }

  switch (static_cast<EventType>(event.type)) {
    case EventType::process_start:
    case EventType::process_exec:
      return event.payload.empty();
    case EventType::standard_output:
    case EventType::standard_error:
      return true;
    case EventType::process_exit:
    case EventType::process_signal:
    case EventType::process_launch_failure:
    case EventType::signal_receive:
      return event.payload.size() == sizeof(std::uint32_t);
  }
  return false;
}

}  // namespace

std::error_code make_error_code(const TraceErrc error) noexcept {
  return {static_cast<int>(error), trace_error_category};
}

bool is_trace_error(const std::error_code& error) noexcept {
  return &error.category() == &trace_error_category;
}

Reader::~Reader() { close(); }

Reader::Reader(Reader&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)),
      header_(std::move(other.header_)),
      last_offset_nanoseconds_(std::exchange(other.last_offset_nanoseconds_, 0U)),
      has_timestamp_(std::exchange(other.has_timestamp_, false)),
      terminal_(std::exchange(other.terminal_, false)),
      terminal_result_(std::exchange(other.terminal_result_, {})) {}

Reader& Reader::operator=(Reader&& other) noexcept {
  if (this != &other) {
    close();
    descriptor_ = std::exchange(other.descriptor_, -1);
    header_ = std::move(other.header_);
    last_offset_nanoseconds_ = std::exchange(other.last_offset_nanoseconds_, 0U);
    has_timestamp_ = std::exchange(other.has_timestamp_, false);
    terminal_ = std::exchange(other.terminal_, false);
    terminal_result_ = std::exchange(other.terminal_result_, {});
  }
  return *this;
}

std::error_code Reader::open(const std::filesystem::path& path, Reader& result) {
  if (path.empty() || path.native().find('\0') != std::string::npos) {
    return std::make_error_code(std::errc::invalid_argument);
  }

  int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (descriptor < 0) {
    return system_error(errno);
  }
  if (const auto error = move_above_standard_descriptors(descriptor)) {
    ::close(descriptor);
    return error;
  }

  Reader candidate;
  candidate.descriptor_ = descriptor;

  struct stat file_status {};
  if (::fstat(candidate.descriptor_, &file_status) < 0) {
    return system_error(errno);
  }
  if (!S_ISREG(file_status.st_mode)) {
    return make_error_code(TraceErrc::not_a_regular_file);
  }

  std::array<std::byte, trace_header_prefix_size> prefix{};
  const auto prefix_read =
      read_exact(candidate.descriptor_, prefix.data(), prefix.size());
  if (prefix_read.error) {
    return prefix_read.error;
  }
  if (prefix_read.bytes_read != prefix.size()) {
    return make_error_code(TraceErrc::truncated_header);
  }
  if (!std::equal(trace_magic.begin(), trace_magic.end(), prefix.begin())) {
    return make_error_code(TraceErrc::invalid_magic);
  }

  candidate.header_.major_version = decode_u16(prefix.data() + 8U);
  candidate.header_.minor_version = decode_u16(prefix.data() + 10U);
  if (candidate.header_.major_version != format_major_version ||
      candidate.header_.minor_version > format_minor_version) {
    return make_error_code(TraceErrc::unsupported_version);
  }

  const auto header_payload_size = decode_u32(prefix.data() + 12U);
  if (header_payload_size > maximum_header_payload_size) {
    return make_error_code(TraceErrc::header_too_large);
  }

  std::vector<std::byte> payload(header_payload_size);
  const auto payload_read =
      read_exact(candidate.descriptor_, payload.data(), payload.size());
  if (payload_read.error) {
    return payload_read.error;
  }
  if (payload_read.bytes_read != payload.size()) {
    return make_error_code(TraceErrc::truncated_header);
  }

  HeaderCursor cursor{payload};
  if (!cursor.read_u64(candidate.header_.realtime_base_nanoseconds) ||
      !cursor.read_u64(candidate.header_.monotonic_base_nanoseconds) ||
      !cursor.read_string(candidate.header_.retrace_version) ||
      !cursor.read_string(candidate.header_.operating_system) ||
      !cursor.read_string(candidate.header_.architecture) ||
      !cursor.read_string(candidate.header_.working_directory)) {
    return make_error_code(TraceErrc::malformed_header);
  }

  std::uint32_t argument_count = 0U;
  if (!cursor.read_u32(argument_count) || argument_count == 0U ||
      argument_count > maximum_argument_count ||
      argument_count > cursor.remaining() / sizeof(std::uint32_t)) {
    return make_error_code(TraceErrc::malformed_header);
  }

  candidate.header_.arguments.reserve(argument_count);
  for (std::uint32_t index = 0U; index < argument_count; ++index) {
    std::string argument;
    if (!cursor.read_string(argument)) {
      return make_error_code(TraceErrc::malformed_header);
    }
    candidate.header_.arguments.push_back(std::move(argument));
  }
  if (cursor.remaining() != 0U) {
    return make_error_code(TraceErrc::malformed_header);
  }

  result = std::move(candidate);
  return {};
}

EventReadResult Reader::next(Event& result) {
  if (terminal_) {
    return terminal_result_;
  }
  if (descriptor_ < 0) {
    return finish({.status = ReadStatus::error,
                   .error = std::make_error_code(std::errc::bad_file_descriptor)});
  }

  std::array<std::byte, sizeof(std::uint32_t)> encoded_frame_size{};
  const auto size_read =
      read_exact(descriptor_, encoded_frame_size.data(), encoded_frame_size.size());
  if (size_read.error) {
    return finish({.status = ReadStatus::error, .error = size_read.error});
  }
  if (size_read.bytes_read == 0U) {
    return finish({.status = ReadStatus::end, .error = {}});
  }
  if (size_read.bytes_read != encoded_frame_size.size()) {
    return finish({.status = ReadStatus::incomplete_final_frame, .error = {}});
  }

  const auto frame_size = decode_u32(encoded_frame_size.data());
  if (frame_size < event_header_after_length_size) {
    return finish({.status = ReadStatus::error,
                   .error = make_error_code(TraceErrc::malformed_frame)});
  }
  if (frame_size > event_header_after_length_size + maximum_event_payload_size) {
    return finish({.status = ReadStatus::error,
                   .error = make_error_code(TraceErrc::frame_too_large)});
  }

  std::array<std::byte, event_header_after_length_size> header{};
  const auto header_read = read_exact(descriptor_, header.data(), header.size());
  if (header_read.error) {
    return finish({.status = ReadStatus::error, .error = header_read.error});
  }
  if (header_read.bytes_read != header.size()) {
    return finish({.status = ReadStatus::incomplete_final_frame, .error = {}});
  }

  Event event{
      .type = decode_u16(header.data()),
      .flags = decode_u16(header.data() + 2U),
      .offset_nanoseconds = decode_u64(header.data() + 4U),
      .process_id = decode_u32(header.data() + 12U),
      .thread_id = decode_u32(header.data() + 16U),
      .payload = {},
  };
  const auto payload_size = decode_u32(header.data() + 20U);
  if (event.flags != 0U ||
      payload_size != frame_size - event_header_after_length_size) {
    return finish({.status = ReadStatus::error,
                   .error = make_error_code(TraceErrc::malformed_frame)});
  }

  event.payload.resize(payload_size);
  const auto payload_read =
      read_exact(descriptor_, event.payload.data(), event.payload.size());
  if (payload_read.error) {
    return finish({.status = ReadStatus::error, .error = payload_read.error});
  }
  if (payload_read.bytes_read != event.payload.size()) {
    return finish({.status = ReadStatus::incomplete_final_frame, .error = {}});
  }

  if (!payload_shape_is_valid(event)) {
    return finish({.status = ReadStatus::error,
                   .error = make_error_code(TraceErrc::invalid_event)});
  }
  if (has_timestamp_ && event.offset_nanoseconds < last_offset_nanoseconds_) {
    return finish({.status = ReadStatus::error,
                   .error = make_error_code(TraceErrc::non_monotonic_timestamp)});
  }
  last_offset_nanoseconds_ = event.offset_nanoseconds;
  has_timestamp_ = true;

  result = std::move(event);
  return {.status = ReadStatus::event, .error = {}};
}

EventReadResult Reader::finish(EventReadResult result) noexcept {
  terminal_ = true;
  terminal_result_ = result;
  return result;
}

void Reader::close() noexcept {
  if (descriptor_ >= 0) {
    ::close(descriptor_);
    descriptor_ = -1;
  }
  header_ = {};
  last_offset_nanoseconds_ = 0U;
  has_timestamp_ = false;
  terminal_ = false;
  terminal_result_ = {};
}

}  // namespace retrace::trace
