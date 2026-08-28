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
  file_open = 10U,
  file_close = 11U,
  runtime_operations_dropped = 12U,
};

// File-event payload limits. The path bound matches the runtime's own limit, so
// a path the runtime already truncated is never truncated a second time here.
inline constexpr std::uint32_t maximum_file_event_path_size = 4096U;
inline constexpr std::uint32_t file_event_header_size = 52U;

// Payload flags are scoped to one file event and are unrelated to the frame
// header's flags field, which stays zero in v1.0. A reader rejects any bit
// outside the defined mask so a later addition cannot be silently misread.
inline constexpr std::uint32_t file_event_flag_path_truncated = 0x1U;
inline constexpr std::uint32_t file_event_flag_relative_to_directory = 0x2U;
inline constexpr std::uint32_t file_event_defined_flags =
    file_event_flag_path_truncated | file_event_flag_relative_to_directory;

// Decoded payload of a `file.open` or `file.close` event. Only the members
// meaningful for the event type carry information; the rest stay zero, so a
// consumer never has to guess which field a given event populated.
//
// The frame's process and thread identifiers are deliberately not members: they
// live in the event frame header, and mixing frame fields into a payload struct
// would let a caller populate one and silently lose the other.
struct FileEvent {
  // Completion time observed inside the target, as an offset from the header's
  // monotonic base. It is not the frame timestamp, which records when the
  // supervisor received the report; under concurrency the two can disagree.
  std::uint64_t completion_offset_nanoseconds = 0U;
  std::uint64_t duration_nanoseconds = 0U;
  // The value the interposed call returned. `error_number` is meaningful only
  // when this is negative.
  std::int64_t result = 0;
  std::uint32_t error_number = 0U;
  // A successful open result, or the argument of a close.
  std::int32_t descriptor = 0;
  // Meaningful only with `file_event_flag_relative_to_directory`.
  std::int32_t directory = 0;
  std::uint32_t open_flags = 0U;
  std::uint32_t mode = 0U;
  std::uint32_t flags = 0U;
  // Borrowed from the payload passed to decode_file_event, and empty for a
  // close. Bytes are stored without a terminator and need not be valid UTF-8.
  std::string_view path;
};

// Decodes and fully validates one file-event payload. Returning false means the
// bytes are not a valid v1.0 file event; `result` is then unspecified. The
// returned path borrows from `payload`, which must outlive every use of it.
[[nodiscard]] bool decode_file_event(EventType type, std::string_view payload,
                                     FileEvent& result);

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
  // `type` must be file_open or file_close. The thread identifier belongs to
  // the target thread that made the call and is stored in the frame header.
  [[nodiscard]] std::error_code write_file_event(EventType type,
                                                 std::uint32_t process_id,
                                                 std::uint32_t thread_id,
                                                 const FileEvent& fields);
  // Records that the bounded runtime channel lost `count` reported operations,
  // so a reader can tell an incomplete record from a complete one.
  [[nodiscard]] std::error_code write_dropped_operations_event(std::uint32_t process_id,
                                                               std::uint64_t count);
  // Converts an absolute CLOCK_MONOTONIC reading, such as the completion time a
  // runtime reported, into the offset this trace stores. A reading older than
  // the header's base saturates at zero rather than wrapping.
  [[nodiscard]] std::uint64_t offset_from_monotonic(
      std::uint64_t monotonic_nanoseconds) const noexcept;
  // Closes the file and reports a final close(2) error. This is idempotent and
  // does not promise fsync(2)-level durability.
  [[nodiscard]] std::error_code finish() noexcept;

  [[nodiscard]] bool is_open() const noexcept { return descriptor_ >= 0; }

 private:
  // Single funnel for every frame: schema validation, timestamping, and the
  // header/payload write pair happen in exactly one place.
  [[nodiscard]] std::error_code write_frame(EventType type, std::uint32_t process_id,
                                            std::uint32_t thread_id,
                                            std::string_view payload);
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
