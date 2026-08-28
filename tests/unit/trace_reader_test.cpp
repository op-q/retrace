// Reader hardening tests built from independently encoded byte vectors. This
// avoids using Writer as the test oracle and exercises every truncation boundary.

#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "retrace/trace.hpp"

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
    path_ = "/tmp/retrace-reader-test-XXXXXX";
    const int descriptor = ::mkstemp(path_.data());
    if (descriptor < 0) {
      path_.clear();
      return;
    }
    ::close(descriptor);
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

void append_u16(std::vector<unsigned char>& bytes, const std::uint16_t value) {
  bytes.push_back(static_cast<unsigned char>(value & 0xffU));
  bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<unsigned char>& bytes, const std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes.push_back(static_cast<unsigned char>((value >> (index * 8U)) & 0xffU));
  }
}

void append_u64(std::vector<unsigned char>& bytes, const std::uint64_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes.push_back(static_cast<unsigned char>((value >> (index * 8U)) & 0xffU));
  }
}

void set_u16(std::vector<unsigned char>& bytes, const std::size_t offset,
             const std::uint16_t value) {
  bytes[offset] = static_cast<unsigned char>(value & 0xffU);
  bytes[offset + 1U] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

void set_u32(std::vector<unsigned char>& bytes, const std::size_t offset,
             const std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
  }
}

void append_string(std::vector<unsigned char>& bytes, const std::string_view value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

[[nodiscard]] std::vector<unsigned char> golden_header() {
  std::vector<unsigned char> payload;
  append_u64(payload, 1000U);
  append_u64(payload, 2000U);
  append_string(payload, "v");
  append_string(payload, "o");
  append_string(payload, "a");
  append_string(payload, "/");
  append_u32(payload, 1U);
  append_string(payload, "cmd");

  std::vector<unsigned char> bytes{'R', 'E', 'T', 'R', 'A', 'C', 'E', 0U};
  append_u16(bytes, retrace::trace::format_major_version);
  append_u16(bytes, retrace::trace::format_minor_version);
  append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void append_frame(std::vector<unsigned char>& bytes, const std::uint16_t type,
                  const std::uint64_t offset_nanoseconds,
                  const std::vector<unsigned char>& payload,
                  const std::uint16_t flags = 0U) {
  append_u32(bytes, 24U + static_cast<std::uint32_t>(payload.size()));
  append_u16(bytes, type);
  append_u16(bytes, flags);
  append_u64(bytes, offset_nanoseconds);
  append_u32(bytes, 42U);
  append_u32(bytes, 0U);
  append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
  bytes.insert(bytes.end(), payload.begin(), payload.end());
}
// NOLINTEND(bugprone-easily-swappable-parameters)

void write_bytes(const std::string& path, const std::vector<unsigned char>& bytes) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::error_code open_bytes(const std::vector<unsigned char>& bytes,
                                         retrace::trace::Reader& reader) {
  TemporaryPath path;
  if (path.value().empty()) {
    return std::make_error_code(std::errc::io_error);
  }
  write_bytes(path.value(), bytes);
  return retrace::trace::Reader::open(path.value(), reader);
}

void expect_open_error(TestContext& test, const std::vector<unsigned char>& bytes,
                       const retrace::trace::TraceErrc expected,
                       const std::string_view message) {
  retrace::trace::Reader reader;
  test.expect(open_bytes(bytes, reader) == retrace::trace::make_error_code(expected),
              message);
}

void test_reads_known_and_unknown_events(TestContext& test) {
  auto bytes = golden_header();
  append_frame(bytes,
               static_cast<std::uint16_t>(retrace::trace::EventType::process_start), 1U,
               {});
  append_frame(bytes, 60000U, 2U, {0U, '\n', 0x1bU, 0xffU});
  std::vector<unsigned char> exit_payload;
  append_u32(exit_payload, 7U);
  append_frame(bytes,
               static_cast<std::uint16_t>(retrace::trace::EventType::process_exit), 3U,
               exit_payload);

  retrace::trace::Reader reader;
  test.expect(!open_bytes(bytes, reader), "the independent golden trace opens");
  test.expect(
      reader.header().major_version == 1U && reader.header().minor_version == 0U,
      "the reader decodes the format version");
  test.expect(reader.header().realtime_base_nanoseconds == 1000U &&
                  reader.header().monotonic_base_nanoseconds == 2000U,
              "the reader decodes both clock bases");
  test.expect(reader.header().retrace_version == "v" &&
                  reader.header().operating_system == "o" &&
                  reader.header().architecture == "a" &&
                  reader.header().working_directory == "/",
              "the reader decodes bounded metadata strings");
  test.expect(
      reader.header().arguments.size() == 1U && reader.header().arguments[0] == "cmd",
      "the reader decodes target arguments");

  retrace::trace::Event event;
  auto read = reader.next(event);
  test.expect(read.status == retrace::trace::ReadStatus::event &&
                  event.type == static_cast<std::uint16_t>(
                                    retrace::trace::EventType::process_start) &&
                  event.payload.empty(),
              "the reader decodes a known empty event");
  read = reader.next(event);
  test.expect(read.status == retrace::trace::ReadStatus::event &&
                  event.type == 60000U && event.payload.size() == 4U &&
                  static_cast<unsigned char>(event.payload[3]) == 0xffU,
              "the reader preserves a valid unknown event and arbitrary bytes");
  read = reader.next(event);
  test.expect(read.status == retrace::trace::ReadStatus::event &&
                  event.type == static_cast<std::uint16_t>(
                                    retrace::trace::EventType::process_exit) &&
                  event.payload.size() == sizeof(std::uint32_t),
              "the reader decodes a known value event");
  read = reader.next(event);
  test.expect(read.status == retrace::trace::ReadStatus::end,
              "exact frame-boundary EOF is structurally complete");
  test.expect(reader.next(event).status == retrace::trace::ReadStatus::end,
              "EOF remains terminal on subsequent reads");
}

// Builds a file-event payload independently of the encoder, so this test cannot
// pass merely because both sides made the same mistake.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
[[nodiscard]] std::vector<unsigned char> file_event_payload(
    const std::int64_t result, const std::uint32_t error_number,
    const std::uint32_t flags, const std::string_view path) {
  std::vector<unsigned char> payload;
  append_u64(payload, 111U);
  append_u64(payload, 222U);
  append_u64(payload, static_cast<std::uint64_t>(result));
  append_u32(payload, error_number);
  append_u32(payload, 3U);
  append_u32(payload, 4U);
  append_u32(payload, 0x241U);
  append_u32(payload, 0600U);
  append_u32(payload, flags);
  append_u32(payload, static_cast<std::uint32_t>(path.size()));
  payload.insert(payload.end(), path.begin(), path.end());
  return payload;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

void expect_invalid_event(TestContext& test, const std::vector<unsigned char>& bytes,
                          const std::string_view message) {
  retrace::trace::Reader reader;
  if (open_bytes(bytes, reader)) {
    test.expect(false, message);
    return;
  }

  retrace::trace::Event event;
  const auto read = reader.next(event);
  test.expect(read.status == retrace::trace::ReadStatus::error &&
                  read.error == retrace::trace::make_error_code(
                                    retrace::trace::TraceErrc::invalid_event),
              message);
}

void test_reads_and_rejects_file_events(TestContext& test) {
  constexpr std::string_view synthetic_path = "/synthetic/observed.txt";
  constexpr auto open_type =
      static_cast<std::uint16_t>(retrace::trace::EventType::file_open);
  constexpr auto close_type =
      static_cast<std::uint16_t>(retrace::trace::EventType::file_close);
  constexpr auto dropped_type =
      static_cast<std::uint16_t>(retrace::trace::EventType::runtime_operations_dropped);

  {
    auto bytes = golden_header();
    append_frame(
        bytes, open_type, 1U,
        file_event_payload(-1, 2U, retrace::trace::file_event_flag_path_truncated,
                           synthetic_path));
    append_frame(bytes, close_type, 2U, file_event_payload(0, 0U, 0U, {}));
    std::vector<unsigned char> dropped_payload;
    append_u64(dropped_payload, 5U);
    append_frame(bytes, dropped_type, 3U, dropped_payload);

    retrace::trace::Reader reader;
    test.expect(!open_bytes(bytes, reader), "the file-event golden trace opens");

    retrace::trace::Event event;
    auto read = reader.next(event);
    retrace::trace::FileEvent file;
    test.expect(read.status == retrace::trace::ReadStatus::event &&
                    retrace::trace::decode_file_event(
                        retrace::trace::EventType::file_open, event.payload, file),
                "the reader accepts an independently encoded file-open event");
    test.expect(file.result == -1 && file.error_number == 2U && file.descriptor == 3 &&
                    file.directory == 4 && file.open_flags == 0x241U &&
                    file.mode == 0600U,
                "every file-open field decodes at its own offset");
    test.expect(file.path == synthetic_path &&
                    (file.flags & retrace::trace::file_event_flag_path_truncated) != 0U,
                "the stored path and its truncation flag decode together");

    read = reader.next(event);
    test.expect(read.status == retrace::trace::ReadStatus::event &&
                    retrace::trace::decode_file_event(
                        retrace::trace::EventType::file_close, event.payload, file) &&
                    file.path.empty(),
                "the reader accepts a close event with no path");

    read = reader.next(event);
    test.expect(read.status == retrace::trace::ReadStatus::event &&
                    event.payload.size() == sizeof(std::uint64_t),
                "the reader accepts a 64-bit dropped-operation count");
  }

  {
    auto payload = file_event_payload(0, 0U, 0U, synthetic_path);
    payload.resize(retrace::trace::file_event_header_size - 1U);
    auto bytes = golden_header();
    append_frame(bytes, open_type, 1U, payload);
    expect_invalid_event(test, bytes,
                         "a payload shorter than the fixed header is rejected");
  }

  {
    auto payload = file_event_payload(0, 0U, 0U, synthetic_path);
    // A length that disagrees with the payload it describes is exactly the input
    // that must never be trusted to address memory.
    set_u32(payload, 48U, static_cast<std::uint32_t>(synthetic_path.size() + 1U));
    auto bytes = golden_header();
    append_frame(bytes, open_type, 1U, payload);
    expect_invalid_event(test, bytes,
                         "a path size larger than the payload is rejected");
  }

  {
    auto payload = file_event_payload(0, 0U, 0U, synthetic_path);
    set_u32(payload, 48U, retrace::trace::maximum_file_event_path_size + 1U);
    auto bytes = golden_header();
    append_frame(bytes, open_type, 1U, payload);
    expect_invalid_event(test, bytes,
                         "a path size above the recorded bound is rejected");
  }

  {
    auto payload = file_event_payload(0, 0U, 0U, synthetic_path);
    set_u32(payload, 44U, ~retrace::trace::file_event_defined_flags);
    auto bytes = golden_header();
    append_frame(bytes, open_type, 1U, payload);
    expect_invalid_event(test, bytes, "an undefined payload flag is rejected");
  }

  {
    auto bytes = golden_header();
    append_frame(bytes, close_type, 1U, file_event_payload(0, 0U, 0U, synthetic_path));
    expect_invalid_event(test, bytes, "a close carrying a path is rejected");
  }

  {
    std::vector<unsigned char> dropped_payload;
    append_u32(dropped_payload, 5U);
    auto bytes = golden_header();
    append_frame(bytes, dropped_type, 1U, dropped_payload);
    expect_invalid_event(test, bytes,
                         "a dropped-operation count of the wrong size is rejected");
  }
}

void test_rejects_malformed_headers(TestContext& test) {
  const auto complete_header = golden_header();
  bool all_header_cuts_are_truncated = true;
  for (std::size_t cut = 0U; cut < complete_header.size(); ++cut) {
    const std::vector<unsigned char> candidate{
        complete_header.begin(),
        complete_header.begin() + static_cast<std::ptrdiff_t>(cut)};
    retrace::trace::Reader cut_reader;
    if (open_bytes(candidate, cut_reader) !=
        retrace::trace::make_error_code(retrace::trace::TraceErrc::truncated_header)) {
      all_header_cuts_are_truncated = false;
      break;
    }
  }
  test.expect(all_header_cuts_are_truncated,
              "every cut through the declared header is classified as truncation");

  expect_open_error(test, {}, retrace::trace::TraceErrc::truncated_header,
                    "an empty file is a truncated header");

  auto truncated_prefix = golden_header();
  truncated_prefix.resize(15U);
  expect_open_error(test, truncated_prefix, retrace::trace::TraceErrc::truncated_header,
                    "a short fixed header is rejected");

  auto wrong_magic = golden_header();
  wrong_magic[7] = 1U;
  expect_open_error(test, wrong_magic, retrace::trace::TraceErrc::invalid_magic,
                    "all eight magic bytes are checked");

  auto wrong_major = golden_header();
  set_u16(wrong_major, 8U, 2U);
  expect_open_error(test, wrong_major, retrace::trace::TraceErrc::unsupported_version,
                    "an unknown major version is rejected");

  auto wrong_minor = golden_header();
  set_u16(wrong_minor, 10U, 1U);
  expect_open_error(test, wrong_minor, retrace::trace::TraceErrc::unsupported_version,
                    "an unknown minor version is rejected conservatively");

  auto oversized = golden_header();
  set_u32(oversized, 12U, retrace::trace::maximum_header_payload_size + 1U);
  oversized.resize(16U);
  expect_open_error(test, oversized, retrace::trace::TraceErrc::header_too_large,
                    "an oversized header is rejected before allocation");

  auto truncated_payload = golden_header();
  truncated_payload.pop_back();
  expect_open_error(test, truncated_payload,
                    retrace::trace::TraceErrc::truncated_header,
                    "a truncated metadata payload is distinct from malformed data");

  auto impossible_string = golden_header();
  set_u32(impossible_string, 32U, std::numeric_limits<std::uint32_t>::max());
  expect_open_error(test, impossible_string,
                    retrace::trace::TraceErrc::malformed_header,
                    "a string length cannot exceed remaining header bytes");

  auto zero_arguments = golden_header();
  set_u32(zero_arguments, 52U, 0U);
  expect_open_error(test, zero_arguments, retrace::trace::TraceErrc::malformed_header,
                    "a trace header requires a target command");

  auto excessive_arguments = golden_header();
  set_u32(excessive_arguments, 52U, retrace::trace::maximum_argument_count + 1U);
  expect_open_error(test, excessive_arguments,
                    retrace::trace::TraceErrc::malformed_header,
                    "argument count is bounded before reserving a vector");

  auto trailing_header_byte = golden_header();
  trailing_header_byte.push_back(0U);
  set_u32(trailing_header_byte, 12U,
          static_cast<std::uint32_t>(trailing_header_byte.size() - 16U));
  expect_open_error(test, trailing_header_byte,
                    retrace::trace::TraceErrc::malformed_header,
                    "v1.0 rejects unparsed trailing header bytes");
}

void test_distinguishes_truncation_and_malformed_frames(TestContext& test) {
  auto partial_length = golden_header();
  partial_length.push_back(24U);
  retrace::trace::Reader reader;
  test.expect(!open_bytes(partial_length, reader),
              "a trace with a partial frame length still opens");
  retrace::trace::Event event;
  test.expect(
      reader.next(event).status == retrace::trace::ReadStatus::incomplete_final_frame,
      "one trailing frame-length byte is recoverable truncation");
  test.expect(
      reader.next(event).status == retrace::trace::ReadStatus::incomplete_final_frame,
      "truncation remains terminal on subsequent reads");

  auto undersized = golden_header();
  append_u32(undersized, 23U);
  test.expect(!open_bytes(undersized, reader), "an undersized-frame trace opens");
  event = {.type = 99U, .payload = "sentinel"};
  const auto malformed_read = reader.next(event);
  test.expect(malformed_read.error == retrace::trace::make_error_code(
                                          retrace::trace::TraceErrc::malformed_frame),
              "a frame shorter than its fixed fields is malformed");
  test.expect(reader.next(event).error == malformed_read.error,
              "a malformed-frame error remains terminal on subsequent reads");
  test.expect(event.type == 99U && event.payload == "sentinel",
              "a failed read does not replace the caller's last valid event");

  auto oversized = golden_header();
  append_u32(oversized, 25U + retrace::trace::maximum_event_payload_size);
  test.expect(!open_bytes(oversized, reader), "an oversized-frame trace opens");
  test.expect(
      reader.next(event).error ==
          retrace::trace::make_error_code(retrace::trace::TraceErrc::frame_too_large),
      "an oversized frame is rejected before allocation");

  auto partial_header = golden_header();
  append_u32(partial_header, 24U);
  partial_header.push_back(1U);
  test.expect(!open_bytes(partial_header, reader),
              "a partial-event-header trace opens");
  test.expect(
      reader.next(event).status == retrace::trace::ReadStatus::incomplete_final_frame,
      "a partial fixed event header is recoverable truncation");

  auto mismatched_sizes = golden_header();
  append_frame(mismatched_sizes, 60000U, 1U, {});
  set_u32(mismatched_sizes, golden_header().size(), 25U);
  test.expect(!open_bytes(mismatched_sizes, reader),
              "a redundant-size-mismatch trace opens");
  test.expect(
      reader.next(event).error ==
          retrace::trace::make_error_code(retrace::trace::TraceErrc::malformed_frame),
      "disagreeing frame and payload sizes are malformed");

  auto nonzero_flags = golden_header();
  append_frame(nonzero_flags, 60000U, 1U, {}, 1U);
  test.expect(!open_bytes(nonzero_flags, reader), "a flagged-event trace opens");
  test.expect(
      reader.next(event).error ==
          retrace::trace::make_error_code(retrace::trace::TraceErrc::malformed_frame),
      "unsupported v1.0 event flags are rejected");

  auto bad_known_payload = golden_header();
  append_frame(bad_known_payload,
               static_cast<std::uint16_t>(retrace::trace::EventType::process_start), 1U,
               {1U});
  test.expect(!open_bytes(bad_known_payload, reader),
              "a bad-known-payload trace opens");
  test.expect(reader.next(event).error == retrace::trace::make_error_code(
                                              retrace::trace::TraceErrc::invalid_event),
              "known event payload shapes are enforced");

  auto bad_signal_payload = golden_header();
  append_frame(bad_signal_payload,
               static_cast<std::uint16_t>(retrace::trace::EventType::signal_receive),
               1U, {});
  test.expect(!open_bytes(bad_signal_payload, reader),
              "a bad signal-receive trace opens");
  test.expect(reader.next(event).error == retrace::trace::make_error_code(
                                              retrace::trace::TraceErrc::invalid_event),
              "signal-receive requires a u32 payload");

  auto bad_runtime_handshake_payload = golden_header();
  append_frame(bad_runtime_handshake_payload,
               static_cast<std::uint16_t>(retrace::trace::EventType::runtime_handshake),
               1U, {1U});
  test.expect(!open_bytes(bad_runtime_handshake_payload, reader),
              "a bad runtime-handshake trace opens");
  test.expect(reader.next(event).error == retrace::trace::make_error_code(
                                              retrace::trace::TraceErrc::invalid_event),
              "runtime-handshake requires an empty payload");

  auto partial_payload = golden_header();
  append_frame(partial_payload, 60000U, 1U, {1U, 2U});
  partial_payload.pop_back();
  test.expect(!open_bytes(partial_payload, reader), "a partial-payload trace opens");
  test.expect(
      reader.next(event).status == retrace::trace::ReadStatus::incomplete_final_frame,
      "a partial payload is recoverable truncation");

  auto decreasing_time = golden_header();
  append_frame(decreasing_time, 60000U, 2U, {});
  append_frame(decreasing_time, 60001U, 1U, {});
  test.expect(!open_bytes(decreasing_time, reader),
              "a decreasing-timestamp trace opens");
  test.expect(reader.next(event).status == retrace::trace::ReadStatus::event,
              "the first monotonic event is accepted");
  test.expect(reader.next(event).error ==
                  retrace::trace::make_error_code(
                      retrace::trace::TraceErrc::non_monotonic_timestamp),
              "decreasing observation timestamps are rejected");
}

void test_rejects_invalid_and_non_regular_paths(TestContext& test) {
  retrace::trace::Reader reader;
  test.expect(retrace::trace::Reader::open({}, reader) ==
                  std::make_error_code(std::errc::invalid_argument),
              "an empty trace path is rejected");

  const std::string path_with_nul{"/tmp/retrace-reader\0ignored", 27U};
  test.expect(retrace::trace::Reader::open(path_with_nul, reader) ==
                  std::make_error_code(std::errc::invalid_argument),
              "an embedded NUL in a trace path is rejected");

  test.expect(retrace::trace::Reader::open("/dev/null", reader) ==
                  retrace::trace::make_error_code(
                      retrace::trace::TraceErrc::not_a_regular_file),
              "a non-regular trace source is rejected without blocking");
}

void test_every_final_frame_cut_preserves_complete_prefix(TestContext& test) {
  const auto header = golden_header();
  auto complete = header;
  append_frame(complete, 60000U, 1U, {1U, 2U, 3U});
  const auto first_frame_end = complete.size();
  append_frame(complete, 60001U, 2U, {4U, 5U, 6U, 7U});

  bool all_cuts_behaved = true;
  for (std::size_t cut = header.size(); cut <= complete.size(); ++cut) {
    const std::vector<unsigned char> candidate{
        complete.begin(), complete.begin() + static_cast<std::ptrdiff_t>(cut)};
    retrace::trace::Reader reader;
    if (open_bytes(candidate, reader)) {
      all_cuts_behaved = false;
      break;
    }

    std::size_t complete_events = 0U;
    retrace::trace::ReadStatus final_status = retrace::trace::ReadStatus::error;
    while (true) {
      retrace::trace::Event event;
      const auto read = reader.next(event);
      if (read.status == retrace::trace::ReadStatus::event) {
        ++complete_events;
        continue;
      }
      final_status = read.status;
      if (read.error) {
        all_cuts_behaved = false;
      }
      break;
    }

    const auto expected_events = cut < first_frame_end ? 0U : 1U;
    const bool at_boundary =
        cut == header.size() || cut == first_frame_end || cut == complete.size();
    const auto expected_status =
        at_boundary ? retrace::trace::ReadStatus::end
                    : retrace::trace::ReadStatus::incomplete_final_frame;
    if (complete_events != expected_events + (cut == complete.size() ? 1U : 0U) ||
        final_status != expected_status) {
      all_cuts_behaved = false;
      break;
    }
  }

  test.expect(all_cuts_behaved,
              "every final-frame cut preserves exactly the complete prefix");
}

}  // namespace

int main() {
  TestContext test;
  test_reads_known_and_unknown_events(test);
  test_reads_and_rejects_file_events(test);
  test_rejects_malformed_headers(test);
  test_distinguishes_truncation_and_malformed_frames(test);
  test_every_final_frame_cut_preserves_complete_prefix(test);
  test_rejects_invalid_and_non_regular_paths(test);
  return test.result();
}
