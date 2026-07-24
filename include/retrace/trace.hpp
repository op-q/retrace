#pragma once

// Public API for RETRACE's append-only binary trace. Writer and Reader are
// move-only descriptor owners; format constants below are compatibility limits.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace retrace::trace {

inline constexpr std::uint16_t format_major_version = 1U;
inline constexpr std::uint16_t format_minor_version = 0U;
inline constexpr std::uint32_t maximum_header_payload_size = 1024U * 1024U;
inline constexpr std::uint32_t maximum_event_payload_size = 1024U * 1024U;
inline constexpr std::uint32_t maximum_argument_count = 4096U;

// The explicit width mirrors the stable u16 field in the on-disk protocol.
enum class EventType : std::uint16_t {  // NOLINT(performance-enum-size)
  process_start = 1U,
  process_exec = 2U,
  standard_output = 3U,
  standard_error = 4U,
  process_exit = 5U,
  process_signal = 6U,
  process_launch_failure = 7U,
  signal_receive = 8U,
  runtime_handshake = 9U,
};

struct Metadata {
  // Views are consumed synchronously by Writer::create and need not outlive it.
  std::string_view retrace_version;
  std::string_view operating_system;
  std::string_view architecture;
  std::string_view working_directory;
  std::span<const std::string_view> arguments;
};

struct CreationResult {
  std::error_code error;
  bool file_created = false;
};

enum class TraceErrc : std::uint8_t {
  invalid_magic = 1U,
  unsupported_version,
  header_too_large,
  truncated_header,
  malformed_header,
  not_a_regular_file,
  frame_too_large,
  malformed_frame,
  invalid_event,
  non_monotonic_timestamp,
};

[[nodiscard]] std::error_code make_error_code(TraceErrc error) noexcept;
[[nodiscard]] bool is_trace_error(const std::error_code& error) noexcept;

struct Header {
  std::uint16_t major_version = 0U;
  std::uint16_t minor_version = 0U;
  std::uint64_t realtime_base_nanoseconds = 0U;
  std::uint64_t monotonic_base_nanoseconds = 0U;
  std::string retrace_version;
  std::string operating_system;
  std::string architecture;
  std::string working_directory;
  std::vector<std::string> arguments;
};

struct Event {
  // Reader owns payload storage here. Unknown type IDs are intentionally kept
  // as raw u16 values so newer traces remain inspectable by older readers.
  std::uint16_t type = 0U;
  std::uint16_t flags = 0U;
  std::uint64_t offset_nanoseconds = 0U;
  std::uint32_t process_id = 0U;
  std::uint32_t thread_id = 0U;
  std::string payload;
};

enum class ReadStatus : std::uint8_t {
  event,
  end,
  incomplete_final_frame,
  error,
};

struct EventReadResult {
  ReadStatus status = ReadStatus::error;
  std::error_code error;
};

// Writer operations are not internally synchronized. Callers must serialize
// them so the separately written frame header and payload cannot interleave.
class Writer final {
 public:
  Writer() = default;
  ~Writer();

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  Writer(Writer&& other) noexcept;
  Writer& operator=(Writer&& other) noexcept;

  [[nodiscard]] static CreationResult create(const std::filesystem::path& path,
                                             const Metadata& metadata, Writer& result);

  [[nodiscard]] std::error_code write_event(EventType type, std::uint32_t process_id,
                                            std::string_view payload = {});
  [[nodiscard]] std::error_code write_value_event(EventType type,
                                                  std::uint32_t process_id,
                                                  std::uint32_t value);
  // Closes the file and reports a final close(2) error. This is idempotent and
  // does not promise fsync(2)-level durability.
  [[nodiscard]] std::error_code finish() noexcept;

  [[nodiscard]] bool is_open() const noexcept { return descriptor_ >= 0; }

 private:
  void close() noexcept;

  int descriptor_ = -1;
  std::uint64_t monotonic_base_nanoseconds_ = 0U;
};

class Reader final {
 public:
  Reader() = default;
  ~Reader();

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  Reader(Reader&& other) noexcept;
  Reader& operator=(Reader&& other) noexcept;

  [[nodiscard]] static std::error_code open(const std::filesystem::path& path,
                                            Reader& result);
  [[nodiscard]] EventReadResult next(Event& result);

  [[nodiscard]] const Header& header() const noexcept { return header_; }

 private:
  [[nodiscard]] EventReadResult finish(EventReadResult result) noexcept;
  void close() noexcept;

  int descriptor_ = -1;
  Header header_;
  std::uint64_t last_offset_nanoseconds_ = 0U;
  bool has_timestamp_ = false;
  bool terminal_ = false;
  EventReadResult terminal_result_;
};

}  // namespace retrace::trace

namespace std {

template <>
struct is_error_code_enum<retrace::trace::TraceErrc> : true_type {};

}  // namespace std
