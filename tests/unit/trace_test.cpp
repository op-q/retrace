// Writer compatibility and failure tests. The cases inspect encoded bytes
// directly so accidental changes to the v1.0 layout cannot hide behind Reader.

#include "retrace/trace.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

class TestContext {
 public:
  void expect(const bool condition, const std::string_view message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << '\n';
      ++failures_;
    }
  }

  [[nodiscard]] int result() const { return failures_ == 0 ? 0 : 1; }

 private:
  int failures_ = 0;
};

class TemporaryPath final {
 public:
  TemporaryPath() {
    path_ = "/tmp/retrace-trace-test-XXXXXX";
    const int descriptor = ::mkstemp(path_.data());
    if (descriptor < 0) {
      path_.clear();
      return;
    }
    ::close(descriptor);
    ::unlink(path_.c_str());
  }

  ~TemporaryPath() {
    if (!path_.empty()) {
      ::unlink(path_.c_str());
    }
  }

  TemporaryPath(const TemporaryPath&) = delete;
  TemporaryPath& operator=(const TemporaryPath&) = delete;

  [[nodiscard]] const std::string& value() const noexcept { return path_; }

 private:
  std::string path_;
};

[[nodiscard]] std::uint16_t read_u16(const std::vector<unsigned char>& bytes,
                                     const std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U])
                                    << 8U);
}

[[nodiscard]] std::uint32_t read_u32(const std::vector<unsigned char>& bytes,
                                     const std::size_t offset) {
  std::uint32_t result = 0U;
  for (std::size_t index = 0; index < sizeof(result); ++index) {
    result |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
  }
  return result;
}

[[nodiscard]] std::uint64_t read_u64(const std::vector<unsigned char>& bytes,
                                     const std::size_t offset) {
  std::uint64_t result = 0U;
  for (std::size_t index = 0; index < sizeof(result); ++index) {
    result |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
  }
  return result;
}

[[nodiscard]] std::string read_string(const std::vector<unsigned char>& bytes,
                                      std::size_t& offset) {
  const auto size = read_u32(bytes, offset);
  offset += sizeof(std::uint32_t);
  const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
  const auto end = begin + static_cast<std::ptrdiff_t>(size);
  offset += size;
  return {begin, end};
}

[[nodiscard]] std::vector<unsigned char> read_file(const std::string& path) {
  std::ifstream input{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::size_t count_complete_frames(
    const std::vector<unsigned char>& bytes) {
  if (bytes.size() < 16U) {
    return 0U;
  }
  std::size_t offset = 16U + read_u32(bytes, 12U);
  std::size_t frame_count = 0U;
  while (offset + sizeof(std::uint32_t) <= bytes.size()) {
    const auto frame_size = read_u32(bytes, offset);
    if (frame_size > bytes.size() - offset - sizeof(std::uint32_t)) {
      break;
    }
    offset += sizeof(std::uint32_t) + frame_size;
    ++frame_count;
  }
  return frame_count;
}

[[nodiscard]] retrace::trace::Metadata metadata_for(
    const std::span<const std::string_view> arguments) {
  return {
      .retrace_version = "0.test",
      .operating_system = "SyntheticOS",
      .architecture = "synthetic-cpu",
      .working_directory = "/synthetic/work",
      .arguments = arguments,
  };
}

void test_versioned_framed_trace(TestContext& test) {
  TemporaryPath path;
  test.expect(!path.value().empty(), "a temporary trace path is available");
  constexpr std::array arguments{std::string_view{"synthetic-program"},
                                 std::string_view{"fake argument"}};
  const retrace::trace::Metadata metadata{
      .retrace_version = "0.test",
      .operating_system = "SyntheticOS",
      .architecture = "synthetic-cpu",
      .working_directory = "/synthetic/work",
      .arguments = arguments,
  };

  {
    retrace::trace::Writer writer;
    const auto creation =
        retrace::trace::Writer::create(path.value(), metadata, writer);
    test.expect(!creation.error, "a new trace file is created");
    test.expect(creation.file_created,
                "successful creation reports that the path now exists");
    test.expect(writer.is_open(), "a successful writer owns the trace descriptor");

    retrace::trace::Writer duplicate;
    const auto duplicate_creation =
        retrace::trace::Writer::create(path.value(), metadata, duplicate);
    test.expect(
        duplicate_creation.error == std::make_error_code(std::errc::file_exists),
        "an existing trace is never overwritten");
    test.expect(!duplicate_creation.file_created,
                "refusing an existing path reports that no file was created");

    const std::string oversized_payload(
        static_cast<std::size_t>(retrace::trace::maximum_event_payload_size) + 1U, 'x');
    test.expect(writer.write_event(retrace::trace::EventType::standard_output, 42U,
                                   oversized_payload) ==
                    std::make_error_code(std::errc::message_size),
                "an oversized event is rejected before writing");
    test.expect(writer.is_open(),
                "rejecting an oversized event leaves the writer usable");

    test.expect(!writer.write_event(retrace::trace::EventType::process_start, 42U),
                "a process-start frame is written");
    test.expect(!writer.write_event(retrace::trace::EventType::standard_output, 42U,
                                    "synthetic output"),
                "a stream frame is written");
    test.expect(
        !writer.write_value_event(retrace::trace::EventType::process_exit, 42U, 7U),
        "a process-exit frame is written");
    test.expect(!writer.finish(), "finishing a complete trace closes it cleanly");
    test.expect(!writer.is_open(), "a finished writer no longer owns a descriptor");
    test.expect(!writer.finish(), "finishing an already closed writer is idempotent");
    test.expect(writer.write_event(retrace::trace::EventType::process_start, 42U) ==
                    std::make_error_code(std::errc::bad_file_descriptor),
                "a finished writer rejects later events");
  }

  struct stat file_status {};
  test.expect(::stat(path.value().c_str(), &file_status) == 0,
              "the completed trace exists");
  test.expect((file_status.st_mode & 0077U) == 0U,
              "the trace grants no group or other permissions");

  const auto bytes = read_file(path.value());
  test.expect(bytes.size() > 16U, "the trace contains a header and events");
  test.expect(std::string{bytes.begin(), bytes.begin() + 7} == "RETRACE",
              "the trace starts with its magic bytes");
  test.expect(bytes[7] == 0U, "the trace magic has an explicit terminator byte");
  test.expect(read_u16(bytes, 8U) == retrace::trace::format_major_version,
              "the trace stores its major format version");
  test.expect(read_u16(bytes, 10U) == retrace::trace::format_minor_version,
              "the trace stores its minor format version");

  std::size_t metadata_offset = 16U;
  test.expect(read_u64(bytes, metadata_offset) > 0U,
              "the header stores a wall-clock base");
  metadata_offset += sizeof(std::uint64_t);
  test.expect(read_u64(bytes, metadata_offset) > 0U,
              "the header stores a monotonic-clock base");
  metadata_offset += sizeof(std::uint64_t);
  test.expect(read_string(bytes, metadata_offset) == "0.test",
              "the header stores the RETRACE version");
  test.expect(read_string(bytes, metadata_offset) == "SyntheticOS",
              "the header stores the operating-system identifier");
  test.expect(read_string(bytes, metadata_offset) == "synthetic-cpu",
              "the header stores the architecture");
  test.expect(read_string(bytes, metadata_offset) == "/synthetic/work",
              "the header stores the selected working directory");
  test.expect(read_u32(bytes, metadata_offset) == arguments.size(),
              "the header stores the argument count");
  metadata_offset += sizeof(std::uint32_t);
  test.expect(read_string(bytes, metadata_offset) == arguments[0],
              "the header stores the executable argument");
  test.expect(read_string(bytes, metadata_offset) == arguments[1],
              "the header preserves an argument containing a space");

  const auto header_payload_size = read_u32(bytes, 12U);
  test.expect(metadata_offset == 16U + header_payload_size,
              "the metadata consumes the declared header payload");
  test.expect(count_complete_frames(bytes) == 3U,
              "all complete event frames can be scanned by length");

  std::size_t frame_offset = metadata_offset;
  test.expect(read_u32(bytes, frame_offset) == 24U,
              "an empty event declares only its fixed fields");
  test.expect(read_u16(bytes, frame_offset + 4U) ==
                  static_cast<std::uint16_t>(retrace::trace::EventType::process_start),
              "the first event stores its stable type identifier");
  test.expect(read_u16(bytes, frame_offset + 6U) == 0U,
              "the event flags are zero in v1.0");
  const auto start_timestamp = read_u64(bytes, frame_offset + 8U);
  test.expect(read_u32(bytes, frame_offset + 16U) == 42U,
              "the event stores its process identifier");
  test.expect(read_u32(bytes, frame_offset + 20U) == 0U,
              "the unobserved thread identifier is zero");
  test.expect(read_u32(bytes, frame_offset + 24U) == 0U,
              "an empty event has a zero payload size");
  frame_offset += sizeof(std::uint32_t) + read_u32(bytes, frame_offset);

  const auto stdout_payload_size = read_u32(bytes, frame_offset + 24U);
  test.expect(read_u32(bytes, frame_offset) == 24U + stdout_payload_size,
              "the frame and payload lengths agree");
  test.expect(
      read_u16(bytes, frame_offset + 4U) ==
          static_cast<std::uint16_t>(retrace::trace::EventType::standard_output),
      "the stream event stores its stable type identifier");
  test.expect(read_u64(bytes, frame_offset + 8U) >= start_timestamp,
              "event timestamps are monotonic offsets");
  const auto stdout_begin =
      bytes.begin() + static_cast<std::ptrdiff_t>(frame_offset + 28U);
  test.expect(std::string{stdout_begin, stdout_begin + static_cast<std::ptrdiff_t>(
                                                           stdout_payload_size)} ==
                  "synthetic output",
              "a stream payload preserves its exact bytes");
  frame_offset += sizeof(std::uint32_t) + read_u32(bytes, frame_offset);

  test.expect(read_u16(bytes, frame_offset + 4U) ==
                  static_cast<std::uint16_t>(retrace::trace::EventType::process_exit),
              "the final event stores the exit type identifier");
  test.expect(read_u32(bytes, frame_offset + 24U) == sizeof(std::uint32_t),
              "a status event stores one u32 payload");
  test.expect(read_u32(bytes, frame_offset + 28U) == 7U,
              "the exit status is little-endian in the payload");
  frame_offset += sizeof(std::uint32_t) + read_u32(bytes, frame_offset);
  test.expect(frame_offset == bytes.size(),
              "the final frame ends at the end of the complete trace");

  auto truncated = bytes;
  truncated.pop_back();
  test.expect(count_complete_frames(truncated) == 2U,
              "an incomplete final frame preserves preceding complete frames");
}

void test_argument_count_boundaries(TestContext& test) {
  {
    TemporaryPath path;
    test.expect(!path.value().empty(),
                "a temporary path is available for empty-argument validation");
    retrace::trace::Writer writer;
    const auto creation =
        retrace::trace::Writer::create(path.value(), metadata_for({}), writer);
    test.expect(creation.error == std::make_error_code(std::errc::message_size),
                "a trace rejects metadata with no executable argument");
    test.expect(!creation.file_created,
                "invalid empty-argument metadata creates no trace file");
    test.expect(!writer.is_open(),
                "invalid empty-argument metadata leaves the writer closed");
    test.expect(::access(path.value().c_str(), F_OK) != 0,
                "empty-argument validation happens before opening the path");
  }

  {
    TemporaryPath path;
    test.expect(!path.value().empty(),
                "a temporary path is available for excessive-argument validation");
    const std::vector<std::string_view> arguments(
        static_cast<std::size_t>(retrace::trace::maximum_argument_count) + 1U,
        "argument");
    retrace::trace::Writer writer;
    const auto creation =
        retrace::trace::Writer::create(path.value(), metadata_for(arguments), writer);
    test.expect(creation.error == std::make_error_code(std::errc::message_size),
                "a trace rejects metadata above the argument-count limit");
    test.expect(!creation.file_created,
                "excessive argument metadata creates no trace file");
    test.expect(!writer.is_open(),
                "excessive argument metadata leaves the writer closed");
    test.expect(::access(path.value().c_str(), F_OK) != 0,
                "argument-count validation happens before opening the path");
  }

  {
    TemporaryPath path;
    test.expect(!path.value().empty(),
                "a temporary path is available for the maximum argument count");
    const std::vector<std::string_view> arguments(
        retrace::trace::maximum_argument_count, "");
    retrace::trace::Writer writer;
    const auto creation =
        retrace::trace::Writer::create(path.value(), metadata_for(arguments), writer);
    test.expect(!creation.error, "metadata at the argument-count limit is accepted");
    test.expect(creation.file_created,
                "accepted maximum-count metadata creates a trace file");
    test.expect(writer.is_open(), "accepted maximum-count metadata opens the writer");
  }
}

void test_event_schema_validation(TestContext& test) {
  TemporaryPath path;
  test.expect(!path.value().empty(),
              "a temporary path is available for event-schema validation");
  constexpr std::array arguments{std::string_view{"synthetic-program"}};

  {
    retrace::trace::Writer writer;
    const auto creation =
        retrace::trace::Writer::create(path.value(), metadata_for(arguments), writer);
    test.expect(!creation.error, "the event-schema trace is created");

    const auto invalid_argument = std::make_error_code(std::errc::invalid_argument);
    test.expect(writer.write_event(retrace::trace::EventType::process_start, 7U,
                                   "unexpected") == invalid_argument,
                "process-start rejects a payload");
    test.expect(writer.write_event(retrace::trace::EventType::process_exec, 7U,
                                   "unexpected") == invalid_argument,
                "process-exec rejects a payload");
    test.expect(writer.write_event(retrace::trace::EventType::runtime_handshake, 7U,
                                   "unexpected") == invalid_argument,
                "runtime-handshake rejects a payload");
    test.expect(writer.write_event(retrace::trace::EventType::process_exit, 7U) ==
                    invalid_argument,
                "process-exit rejects an empty value payload");
    test.expect(writer.write_event(retrace::trace::EventType::process_signal, 7U,
                                   "bad") == invalid_argument,
                "process-signal rejects a value payload of the wrong size");
    test.expect(writer.write_event(retrace::trace::EventType::process_launch_failure,
                                   7U, "wrong") == invalid_argument,
                "process-launch-failure rejects a value payload of the wrong size");
    test.expect(writer.write_event(retrace::trace::EventType::signal_receive, 7U,
                                   "bad") == invalid_argument,
                "signal-receive rejects a value payload of the wrong size");
    test.expect(writer.write_value_event(retrace::trace::EventType::standard_output, 7U,
                                         1U) == invalid_argument,
                "write_value_event rejects a stream event type");
    test.expect(writer.write_value_event(retrace::trace::EventType::process_start, 7U,
                                         1U) == invalid_argument,
                "write_value_event rejects an empty-payload event type");
    test.expect(writer.is_open(), "schema validation failures leave the writer usable");

    test.expect(!writer.write_event(retrace::trace::EventType::standard_output, 7U),
                "a stream event permits an empty byte payload");
    test.expect(!writer.write_event(retrace::trace::EventType::runtime_handshake, 7U),
                "a runtime handshake permits its required empty payload");
    test.expect(
        !writer.write_value_event(retrace::trace::EventType::process_signal, 7U, 9U),
        "write_value_event accepts a value-bearing event type");
    test.expect(
        !writer.write_value_event(retrace::trace::EventType::signal_receive, 7U, 15U),
        "write_value_event accepts signal-receive evidence");
  }

  const auto bytes = read_file(path.value());
  test.expect(count_complete_frames(bytes) == 4U,
              "rejected event shapes append no frames");
}

void test_event_payload_size_boundary(TestContext& test) {
  TemporaryPath path;
  test.expect(!path.value().empty(),
              "a temporary path is available for payload-boundary validation");
  constexpr std::array arguments{std::string_view{"synthetic-program"}};

  {
    retrace::trace::Writer writer;
    const auto creation =
        retrace::trace::Writer::create(path.value(), metadata_for(arguments), writer);
    test.expect(!creation.error, "the payload-boundary trace is created");

    const std::string maximum_payload(retrace::trace::maximum_event_payload_size, 'x');
    test.expect(!writer.write_event(retrace::trace::EventType::standard_error, 8U,
                                    maximum_payload),
                "an event payload at the size limit is accepted");

    const std::string oversized_payload(
        static_cast<std::size_t>(retrace::trace::maximum_event_payload_size) + 1U, 'x');
    test.expect(writer.write_event(retrace::trace::EventType::standard_error, 8U,
                                   oversized_payload) ==
                    std::make_error_code(std::errc::message_size),
                "an event payload above the size limit is rejected");
    test.expect(writer.is_open(),
                "payload-size validation failure leaves the writer usable");
  }

  const auto bytes = read_file(path.value());
  test.expect(count_complete_frames(bytes) == 1U,
              "only the boundary-sized payload is appended");
}

// Locates the payload of the first event frame. The header payload size is
// stored at a fixed offset, so the first frame begins immediately after it.
[[nodiscard]] std::size_t first_frame_offset(const std::vector<unsigned char>& bytes) {
  return 16U + read_u32(bytes, 12U);
}

void test_file_event_encoding(TestContext& test) {
  TemporaryPath path;
  test.expect(!path.value().empty(),
              "a temporary path is available for file-event encoding");
  constexpr std::array arguments{std::string_view{"synthetic-program"}};
  constexpr std::string_view synthetic_path = "/synthetic/observed.txt";

  const retrace::trace::FileEvent open_fields{
      .completion_offset_nanoseconds = 111U,
      .duration_nanoseconds = 222U,
      .result = 7,
      .error_number = 0U,
      .descriptor = 7,
      .directory = 5,
      .open_flags = 0x241U,
      .mode = 0600U,
      .flags = retrace::trace::file_event_flag_relative_to_directory,
      .path = synthetic_path,
  };

  {
    retrace::trace::Writer writer;
    const auto creation =
        retrace::trace::Writer::create(path.value(), metadata_for(arguments), writer);
    test.expect(!creation.error, "the file-event trace is created");

    test.expect(!writer.write_file_event(retrace::trace::EventType::file_open, 42U, 99U,
                                         open_fields),
                "a file-open frame is written");

    const auto invalid_argument = std::make_error_code(std::errc::invalid_argument);
    test.expect(writer.write_file_event(retrace::trace::EventType::process_start, 42U,
                                        99U, open_fields) == invalid_argument,
                "write_file_event rejects an event type that is not a file event");
    test.expect(writer.write_file_event(retrace::trace::EventType::file_close, 42U, 99U,
                                        open_fields) == invalid_argument,
                "a close carrying a path is rejected");

    retrace::trace::FileEvent undefined_flag = open_fields;
    undefined_flag.flags = ~retrace::trace::file_event_defined_flags;
    test.expect(writer.write_file_event(retrace::trace::EventType::file_open, 42U, 99U,
                                        undefined_flag) == invalid_argument,
                "an undefined payload flag is rejected");

    const std::string oversized(
        static_cast<std::size_t>(retrace::trace::maximum_file_event_path_size) + 1U,
        'p');
    retrace::trace::FileEvent oversized_path = open_fields;
    oversized_path.path = oversized;
    test.expect(writer.write_file_event(retrace::trace::EventType::file_open, 42U, 99U,
                                        oversized_path) ==
                    std::make_error_code(std::errc::message_size),
                "a path above the recorded bound is rejected rather than truncated");
    test.expect(writer.is_open(), "rejected file events leave the writer usable");
  }

  const auto bytes = read_file(path.value());
  test.expect(count_complete_frames(bytes) == 1U,
              "rejected file events append no frames");

  const auto frame = first_frame_offset(bytes);
  const auto payload = frame + 28U;
  test.expect(read_u16(bytes, frame + 4U) ==
                  static_cast<std::uint16_t>(retrace::trace::EventType::file_open),
              "the frame stores the file-open event identifier");
  test.expect(read_u32(bytes, frame + 20U) == 99U,
              "the frame stores the reporting target thread identifier");
  test.expect(read_u32(bytes, frame + 24U) ==
                  retrace::trace::file_event_header_size + synthetic_path.size(),
              "the payload size covers the fixed header and the stored path");

  // Each field is read back at its own offset so a layout change fails here
  // rather than silently producing plausible values.
  test.expect(read_u64(bytes, payload + 0U) == 111U, "the completion offset is stored");
  test.expect(read_u64(bytes, payload + 8U) == 222U, "the duration is stored");
  test.expect(read_u64(bytes, payload + 16U) == 7U, "the call result is stored");
  test.expect(read_u32(bytes, payload + 24U) == 0U, "the error number is stored");
  test.expect(read_u32(bytes, payload + 28U) == 7U, "the descriptor is stored");
  test.expect(read_u32(bytes, payload + 32U) == 5U,
              "the directory descriptor is stored");
  test.expect(read_u32(bytes, payload + 36U) == 0x241U, "the open flags are stored");
  test.expect(read_u32(bytes, payload + 40U) == 0600U, "the creation mode is stored");
  test.expect(read_u32(bytes, payload + 44U) ==
                  retrace::trace::file_event_flag_relative_to_directory,
              "the payload flags are stored");
  test.expect(read_u32(bytes, payload + 48U) == synthetic_path.size(),
              "the path size is stored");

  const auto* const stored_path = reinterpret_cast<const char*>(
      bytes.data() + payload + retrace::trace::file_event_header_size);
  test.expect(std::string_view{stored_path, synthetic_path.size()} == synthetic_path,
              "the path bytes follow the fixed payload header");

  // A negative result must survive the round trip as its exact value, which is
  // what lets a consumer tell a failed call from a descriptor of zero.
  retrace::trace::FileEvent decoded;
  const std::string_view encoded_payload{
      reinterpret_cast<const char*>(bytes.data() + payload),
      retrace::trace::file_event_header_size + synthetic_path.size()};
  test.expect(retrace::trace::decode_file_event(retrace::trace::EventType::file_open,
                                                encoded_payload, decoded),
              "the writer's own bytes decode as a valid file event");
  test.expect(decoded.result == 7 && decoded.path == synthetic_path,
              "decoding recovers the encoded result and path");
}

void test_close_and_dropped_operation_events(TestContext& test) {
  TemporaryPath path;
  test.expect(!path.value().empty(),
              "a temporary path is available for close and drop events");
  constexpr std::array arguments{std::string_view{"synthetic-program"}};

  const retrace::trace::FileEvent close_fields{
      .completion_offset_nanoseconds = 10U,
      .duration_nanoseconds = 20U,
      .result = -1,
      .error_number = 9U,
      .descriptor = -1,
      .directory = 0,
      .open_flags = 0U,
      .mode = 0U,
      .flags = 0U,
      .path = {},
  };

  {
    retrace::trace::Writer writer;
    const auto creation =
        retrace::trace::Writer::create(path.value(), metadata_for(arguments), writer);
    test.expect(!creation.error, "the close-event trace is created");
    test.expect(!writer.write_file_event(retrace::trace::EventType::file_close, 42U,
                                         42U, close_fields),
                "a file-close frame is written");
    test.expect(!writer.write_dropped_operations_event(42U, 5U),
                "a dropped-operation frame is written");
  }

  const auto bytes = read_file(path.value());
  test.expect(count_complete_frames(bytes) == 2U, "both closing frames are appended");

  const auto frame = first_frame_offset(bytes);
  const auto payload = frame + 28U;
  test.expect(read_u32(bytes, frame + 24U) == retrace::trace::file_event_header_size,
              "a close payload is exactly the fixed header");
  test.expect(read_u32(bytes, payload + 48U) == 0U, "a close stores no path");

  retrace::trace::FileEvent decoded;
  const std::string_view close_payload{
      reinterpret_cast<const char*>(bytes.data() + payload),
      retrace::trace::file_event_header_size};
  test.expect(retrace::trace::decode_file_event(retrace::trace::EventType::file_close,
                                                close_payload, decoded),
              "a close payload decodes as a valid file event");
  test.expect(decoded.result == -1 && decoded.descriptor == -1,
              "negative values survive the two's complement round trip");

  const auto drop_frame = frame + 4U + read_u32(bytes, frame);
  test.expect(read_u16(bytes, drop_frame + 4U) ==
                  static_cast<std::uint16_t>(
                      retrace::trace::EventType::runtime_operations_dropped),
              "the closing frame records dropped operations");
  test.expect(read_u64(bytes, drop_frame + 28U) == 5U,
              "the dropped-operation count is stored as a 64-bit value");
}

}  // namespace

int main() {
  TestContext test;
  test_versioned_framed_trace(test);
  test_argument_count_boundaries(test);
  test_event_schema_validation(test);
  test_event_payload_size_boundary(test);
  test_file_event_encoding(test);
  test_close_and_dropped_operation_events(test);
  return test.result();
}
