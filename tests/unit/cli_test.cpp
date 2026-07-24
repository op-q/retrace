// Tests the CLI as a library by replacing terminal streams with string streams.
// It covers exact diagnostics, trace creation, inspection, validation, and the
// mapping between process outcomes and public exit codes.

#include "retrace/cli.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "retrace/trace.hpp"
#include "retrace/version.hpp"

#ifndef RETRACE_SIGNAL_FIXTURE_PATH
#error "RETRACE_SIGNAL_FIXTURE_PATH must name the signal fixture executable"
#endif

#ifndef RETRACE_RUNTIME_CHANNEL_FIXTURE_PATH
#error "RETRACE_RUNTIME_CHANNEL_FIXTURE_PATH must name the runtime channel fixture"
#endif

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
    path_ = "/tmp/retrace-cli-test-XXXXXX";
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

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    path_ = "/tmp/retrace-cli-directory-XXXXXX";
    if (::mkdtemp(path_.data()) == nullptr) {
      path_.clear();
    }
  }

  ~TemporaryDirectory() {
    if (!path_.empty()) {
      ::rmdir(path_.c_str());
    }
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::string& value() const noexcept { return path_; }

 private:
  std::string path_;
};

class SyncFailingStreamBuffer final : public std::stringbuf {
 protected:
  int sync() override { return -1; }
};

struct TraceFrame {
  std::uint16_t type = 0U;
  std::string payload;
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

[[nodiscard]] std::vector<TraceFrame> read_trace_frames(const std::string& path) {
  std::ifstream input{path, std::ios::binary};
  const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>{input},
                                         std::istreambuf_iterator<char>{}};
  std::vector<TraceFrame> frames;
  if (bytes.size() < 16U) {
    return frames;
  }

  std::size_t offset = 16U + read_u32(bytes, 12U);
  while (offset + 28U <= bytes.size()) {
    const auto frame_size = read_u32(bytes, offset);
    const auto payload_size = read_u32(bytes, offset + 24U);
    if (frame_size != 24U + payload_size ||
        frame_size > bytes.size() - offset - sizeof(std::uint32_t)) {
      break;
    }
    const auto payload_offset = offset + 28U;
    frames.push_back(
        {.type = read_u16(bytes, offset + 4U),
         .payload = {bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset),
                     bytes.begin() +
                         static_cast<std::ptrdiff_t>(payload_offset + payload_size)}});
    offset += sizeof(std::uint32_t) + frame_size;
  }
  return frames;
}

[[nodiscard]] bool write_inspection_trace(TestContext& test, const std::string& path) {
  constexpr std::array target_arguments{std::string_view{"demo"},
                                        std::string_view{"arg\0tail", 8U}};
  const retrace::trace::Metadata metadata{
      .retrace_version = "unit\n\"version",
      .operating_system = "Test\tOS",
      .architecture = "arch\\64",
      .working_directory = "/synthetic\npath",
      .arguments = target_arguments,
  };

  retrace::trace::Writer writer;
  const auto creation = retrace::trace::Writer::create(path, metadata, writer);
  test.expect(!creation.error, "the synthetic CLI trace is created");
  if (creation.error) {
    return false;
  }

  const std::string stream_payload{"line\n\t\"\\\0\x01\xff", 11U};
  const std::string unknown_payload{"opaque\0\n", 8U};
  const auto start_error =
      writer.write_event(retrace::trace::EventType::process_start, 42U);
  const auto output_error = writer.write_event(
      retrace::trace::EventType::standard_output, 42U, stream_payload);
  // This deliberately constructs a future event identifier within the u16 field.
  // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
  const auto unknown_error = writer.write_event(
      static_cast<retrace::trace::EventType>(60000U), 42U, unknown_payload);
  // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
  const auto exit_error =
      writer.write_value_event(retrace::trace::EventType::process_exit, 42U, 9U);
  test.expect(!start_error && !output_error && !unknown_error && !exit_error,
              "the synthetic CLI events are written");
  return !start_error && !output_error && !unknown_error && !exit_error;
}

void test_empty_arguments_show_help(TestContext& test) {
  std::ostringstream output;
  std::ostringstream error;

  const auto result = retrace::cli::run({}, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::success),
              "empty arguments return success");
  test.expect(output.str().starts_with("Usage: retrace"), "empty arguments print help");
  test.expect(error.str().empty(), "empty arguments do not print an error");
}

void test_version(TestContext& test) {
  constexpr std::array arguments{std::string_view{"version"}};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::success),
              "version returns success");
  test.expect(output.str() == "retrace " + std::string{retrace::version} + "\n",
              "version prints the configured project version");
  test.expect(error.str().empty(), "version does not print an error");
}

void test_unknown_command(TestContext& test) {
  constexpr std::array arguments{std::string_view{"unknown"}};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::usage_error),
              "an unknown command returns a usage error");
  test.expect(output.str().empty(), "an unknown command has no normal output");
  test.expect(error.str().find("unknown command 'unknown'") != std::string::npos,
              "an unknown command explains the error");
}

void test_version_rejects_extra_arguments(TestContext& test) {
  constexpr std::array arguments{std::string_view{"version"},
                                 std::string_view{"extra"}};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::usage_error),
              "version with extra arguments returns a usage error");
  test.expect(output.str().empty(),
              "version with extra arguments has no normal output");
  test.expect(error.str().find("does not accept arguments") != std::string::npos,
              "version with extra arguments explains the error");
}

void test_run_records_a_forwarded_signal(TestContext& test) {
  TemporaryPath trace_path;
  test.expect(!trace_path.value().empty(),
              "a temporary signal trace path is available");
  if (trace_path.value().empty()) {
    return;
  }

  const std::array arguments{std::string_view{"run"},
                             std::string_view{"--output"},
                             std::string_view{trace_path.value()},
                             std::string_view{"--"},
                             std::string_view{RETRACE_SIGNAL_FIXTURE_PATH},
                             std::string_view{"--request-term"}};
  std::ostringstream output;
  std::ostringstream error;
  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::success),
              "run succeeds when the target group handles a forwarded signal");
  test.expect(output.str() == "ready\nforwarded\n",
              "run forwards output around the handled signal");
  test.expect(error.str().empty(),
              "a handled forwarded signal produces no run diagnostic");

  const auto frames = read_trace_frames(trace_path.value());
  const auto signal_frame =
      std::find_if(frames.begin(), frames.end(), [](const TraceFrame& frame) {
        return frame.type ==
               static_cast<std::uint16_t>(retrace::trace::EventType::signal_receive);
      });
  test.expect(signal_frame != frames.end(),
              "the trace records the signal received for forwarding");
  if (signal_frame != frames.end()) {
    test.expect(signal_frame->payload.size() == sizeof(std::uint32_t) &&
                    static_cast<unsigned char>(signal_frame->payload[0]) == SIGTERM,
                "the signal trace event preserves SIGTERM");
  }

  const std::array inspect_arguments{std::string_view{"inspect"},
                                     std::string_view{trace_path.value()}};
  std::ostringstream inspect_output;
  std::ostringstream inspect_error;
  const auto inspect_result =
      retrace::cli::run(inspect_arguments, inspect_output, inspect_error);
  test.expect(inspect_result == static_cast<int>(retrace::cli::ExitCode::success) &&
                  inspect_output.str().find("signal.receive") != std::string::npos &&
                  inspect_output.str().find("signal=15") != std::string::npos,
              "inspect renders the forwarded-signal evidence");
  test.expect(inspect_error.str().empty(),
              "inspection of signal evidence has no diagnostic");
}

void test_trace_commands_require_exactly_one_path(TestContext& test) {
  constexpr std::array inspect_arguments{std::string_view{"inspect"}};
  std::ostringstream inspect_output;
  std::ostringstream inspect_error;

  const auto inspect_result =
      retrace::cli::run(inspect_arguments, inspect_output, inspect_error);

  test.expect(inspect_result == static_cast<int>(retrace::cli::ExitCode::usage_error),
              "inspect without a path returns a usage error");
  test.expect(inspect_output.str().empty(),
              "invalid inspect syntax has no normal output");
  test.expect(inspect_error.str() == "usage: retrace inspect TRACE\n",
              "inspect prints its exact usage");

  constexpr std::array validate_arguments{std::string_view{"validate"},
                                          std::string_view{"first"},
                                          std::string_view{"second"}};
  std::ostringstream validate_output;
  std::ostringstream validate_error;

  const auto validate_result =
      retrace::cli::run(validate_arguments, validate_output, validate_error);

  test.expect(validate_result == static_cast<int>(retrace::cli::ExitCode::usage_error),
              "validate with extra paths returns a usage error");
  test.expect(validate_output.str().empty(),
              "invalid validate syntax has no normal output");
  test.expect(validate_error.str() == "usage: retrace validate TRACE\n",
              "validate prints its exact usage");
}

void test_trace_commands_report_a_missing_file(TestContext& test) {
  TemporaryPath missing_path;
  test.expect(!missing_path.value().empty(),
              "a missing temporary trace path is available");

  const std::array inspect_arguments{std::string_view{"inspect"},
                                     std::string_view{missing_path.value()}};
  std::ostringstream inspect_output;
  std::ostringstream inspect_error;
  const auto inspect_result =
      retrace::cli::run(inspect_arguments, inspect_output, inspect_error);

  test.expect(
      inspect_result == static_cast<int>(retrace::cli::ExitCode::internal_error),
      "inspect maps a missing file to an operating-system error");
  test.expect(inspect_output.str().empty(),
              "a missing trace produces no inspection output");
  test.expect(
      inspect_error.str().starts_with("error: trace could not be opened\ncause: "),
      "inspect identifies a trace open failure");

  const std::array validate_arguments{std::string_view{"validate"},
                                      std::string_view{missing_path.value()}};
  std::ostringstream validate_output;
  std::ostringstream validate_error;
  const auto validate_result =
      retrace::cli::run(validate_arguments, validate_output, validate_error);

  test.expect(
      validate_result == static_cast<int>(retrace::cli::ExitCode::internal_error),
      "validate maps a missing file to an operating-system error");
  test.expect(validate_output.str().empty(),
              "a missing trace has no validation output");
  test.expect(
      validate_error.str().starts_with("error: trace could not be opened\ncause: "),
      "validate identifies a trace open failure");
}

void test_validate_accepts_a_structurally_valid_trace(TestContext& test) {
  TemporaryPath trace_path;
  test.expect(!trace_path.value().empty(),
              "a temporary validation trace path is available");
  if (trace_path.value().empty() || !write_inspection_trace(test, trace_path.value())) {
    return;
  }

  const std::array arguments{std::string_view{"validate"},
                             std::string_view{trace_path.value()}};
  std::ostringstream output;
  std::ostringstream error;
  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::success),
              "validate accepts a structurally valid trace");
  test.expect(output.str() == "trace is valid\n",
              "validate prints its exact success message");
  test.expect(error.str().empty(), "validating a valid trace has no diagnostic");
}

void test_inspect_renders_safe_metadata_and_timeline(TestContext& test) {
  TemporaryPath trace_path;
  test.expect(!trace_path.value().empty(),
              "a temporary inspection trace path is available");
  if (trace_path.value().empty() || !write_inspection_trace(test, trace_path.value())) {
    return;
  }

  const std::array arguments{std::string_view{"inspect"},
                             std::string_view{trace_path.value()}};
  std::ostringstream output;
  std::ostringstream error;
  const auto result = retrace::cli::run(arguments, output, error);
  const auto rendered = output.str();

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::success),
              "inspect accepts a structurally valid trace");
  test.expect(error.str().empty(), "inspecting a valid trace has no diagnostic");
  test.expect(rendered.starts_with("TRACE version=1.0\n"),
              "inspect renders the trace format version first");
  test.expect(rendered.find("RECORDER \"unit\\n\\\"version\"\n") != std::string::npos,
              "inspect escapes control bytes in recorder metadata");
  test.expect(rendered.find("SYSTEM \"Test\\tOS\" architecture=\"arch\\\\64\"\n") !=
                  std::string::npos,
              "inspect escapes system metadata");
  test.expect(rendered.find("COMMAND \"demo\" \"arg\\0tail\"\n") != std::string::npos,
              "inspect preserves and escapes embedded NUL command bytes");
  test.expect(
      rendered.find("WORKING_DIRECTORY \"/synthetic\\npath\"\n") != std::string::npos,
      "inspect escapes working-directory metadata");
  test.expect(rendered.find("process.start") != std::string::npos &&
                  rendered.find("pid=42") != std::string::npos,
              "inspect renders process lifecycle events");
  test.expect(rendered.find("bytes=11 preview=\"line\\n\\t\\\"\\\\\\0\\x01\\xff\"") !=
                  std::string::npos,
              "inspect escapes arbitrary stream bytes without emitting them raw");
  test.expect(
      rendered.find("unknown(60000) pid=42 tid=0 bytes=8 preview=\"opaque\\0\\n\"") !=
          std::string::npos,
      "inspect preserves unknown event IDs and safely previews their bytes");
  test.expect(rendered.find("process.exit") != std::string::npos &&
                  rendered.find("code=9") != std::string::npos,
              "inspect decodes the process result");
  test.expect(rendered.find("SUMMARY events=4 ") != std::string::npos &&
                  rendered.ends_with("result=exit(9) status=structurally-valid\n"),
              "inspect summarizes all complete events and structural validity");
}

void test_validate_reports_a_normal_output_failure(TestContext& test) {
  TemporaryPath trace_path;
  test.expect(!trace_path.value().empty(),
              "a temporary validation output-failure trace path is available");
  if (trace_path.value().empty() || !write_inspection_trace(test, trace_path.value())) {
    return;
  }

  const std::array arguments{std::string_view{"validate"},
                             std::string_view{trace_path.value()}};
  SyncFailingStreamBuffer output_buffer;
  std::ostream output{&output_buffer};
  std::ostringstream error;
  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::internal_error),
              "validate does not report success when normal output cannot flush");
  test.expect(output_buffer.str() == "trace is valid\n",
              "validate retains its exact success-output contract before flushing");
  test.expect(error.str() ==
                  "error: command output could not be written\n"
                  "cause: normal output stream failure\n",
              "validate reports a useful normal-output diagnostic");
}

void test_inspect_reports_a_normal_output_failure(TestContext& test) {
  TemporaryPath trace_path;
  test.expect(!trace_path.value().empty(),
              "a temporary inspection output-failure trace path is available");
  if (trace_path.value().empty() || !write_inspection_trace(test, trace_path.value())) {
    return;
  }

  const std::array arguments{std::string_view{"inspect"},
                             std::string_view{trace_path.value()}};
  SyncFailingStreamBuffer output_buffer;
  std::ostream output{&output_buffer};
  std::ostringstream error;
  const auto result = retrace::cli::run(arguments, output, error);
  const auto rendered = output_buffer.str();

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::internal_error),
              "inspect does not report success when rendered output cannot flush");
  test.expect(rendered.starts_with("TRACE version=1.0\n") &&
                  rendered.find("stdout.chunk") != std::string::npos &&
                  rendered.find("preview=\"line\\n\\t\\\"\\\\\\0\\x01\\xff\"") !=
                      std::string::npos &&
                  rendered.ends_with("status=structurally-valid\n"),
              "inspect safely renders the complete timeline before flush failure");
  test.expect(error.str() ==
                  "error: command output could not be written\n"
                  "cause: normal output stream failure\n",
              "inspect reports a useful normal-output diagnostic");
}

void test_trace_commands_reject_a_malformed_header(TestContext& test) {
  TemporaryPath trace_path;
  test.expect(!trace_path.value().empty(),
              "a temporary malformed trace path is available");
  if (trace_path.value().empty()) {
    return;
  }
  {
    std::ofstream trace{trace_path.value(), std::ios::binary};
    trace.write("NOTTRACE........", 16);
  }

  for (const auto command :
       {std::string_view{"inspect"}, std::string_view{"validate"}}) {
    const std::array arguments{command, std::string_view{trace_path.value()}};
    std::ostringstream output;
    std::ostringstream error;
    const auto result = retrace::cli::run(arguments, output, error);

    test.expect(result == static_cast<int>(retrace::cli::ExitCode::trace_format_error),
                "a malformed header returns the trace-format error code");
    test.expect(output.str().empty(),
                "a malformed header has no normal command output");
    test.expect(error.str() ==
                    "error: invalid trace\n"
                    "cause: invalid trace magic\n",
                "a malformed header is identified as invalid trace data");
  }
}

void test_trace_commands_reject_an_unsupported_version(TestContext& test) {
  TemporaryPath trace_path;
  test.expect(!trace_path.value().empty(),
              "a temporary unsupported-version path is available");
  if (trace_path.value().empty() || !write_inspection_trace(test, trace_path.value())) {
    return;
  }
  {
    std::fstream trace{trace_path.value(),
                       std::ios::binary | std::ios::in | std::ios::out};
    trace.seekp(10);
    constexpr char unsupported_minor = 1;
    trace.write(&unsupported_minor, 1);
  }

  for (const auto command :
       {std::string_view{"inspect"}, std::string_view{"validate"}}) {
    const std::array arguments{command, std::string_view{trace_path.value()}};
    std::ostringstream output;
    std::ostringstream error;
    const auto result = retrace::cli::run(arguments, output, error);

    test.expect(result == static_cast<int>(retrace::cli::ExitCode::trace_format_error),
                "an unsupported version returns the trace-format error code");
    test.expect(output.str().empty(),
                "an unsupported version produces no normal output");
    test.expect(error.str() ==
                    "error: invalid trace\n"
                    "cause: unsupported trace version\n",
                "an unsupported version has a precise diagnostic");
  }
}

void test_trace_commands_reject_a_non_regular_source(TestContext& test) {
  for (const auto command :
       {std::string_view{"inspect"}, std::string_view{"validate"}}) {
    constexpr std::string_view null_device = "/dev/null";
    const std::array arguments{command, null_device};
    std::ostringstream output;
    std::ostringstream error;
    const auto result = retrace::cli::run(arguments, output, error);

    test.expect(result == static_cast<int>(retrace::cli::ExitCode::trace_format_error),
                "a non-regular source returns the trace-format error code");
    test.expect(output.str().empty(), "a non-regular source produces no normal output");
    test.expect(error.str() ==
                    "error: invalid trace\n"
                    "cause: trace path is not a regular file\n",
                "a non-regular source has a precise diagnostic");
  }
}

void test_malformed_event_reports_the_complete_prefix(TestContext& test) {
  TemporaryPath trace_path;
  test.expect(!trace_path.value().empty(),
              "a temporary malformed-event path is available");
  if (trace_path.value().empty() || !write_inspection_trace(test, trace_path.value())) {
    return;
  }
  {
    std::ofstream trace{trace_path.value(), std::ios::binary | std::ios::app};
    constexpr std::array<char, 4> undersized_frame{23, 0, 0, 0};
    trace.write(undersized_frame.data(),
                static_cast<std::streamsize>(undersized_frame.size()));
  }

  const std::array inspect_arguments{std::string_view{"inspect"},
                                     std::string_view{trace_path.value()}};
  std::ostringstream inspect_output;
  std::ostringstream inspect_error;
  const auto inspect_result =
      retrace::cli::run(inspect_arguments, inspect_output, inspect_error);

  test.expect(
      inspect_result == static_cast<int>(retrace::cli::ExitCode::trace_format_error),
      "inspect maps a malformed later event to the trace-format error code");
  test.expect(inspect_output.str().find("process.start") != std::string::npos &&
                  inspect_output.str().find("process.exit") != std::string::npos,
              "inspect renders complete events before malformed data");
  test.expect(inspect_output.str().find("SUMMARY") == std::string::npos,
              "inspect does not summarize a malformed event stream as complete");
  test.expect(inspect_error.str() ==
                  "error: trace is malformed\n"
                  "cause: trace event frame is malformed\n"
                  "complete events: 4\n",
              "inspect reports the complete prefix length for malformed data");

  const std::array validate_arguments{std::string_view{"validate"},
                                      std::string_view{trace_path.value()}};
  std::ostringstream validate_output;
  std::ostringstream validate_error;
  const auto validate_result =
      retrace::cli::run(validate_arguments, validate_output, validate_error);

  test.expect(
      validate_result == static_cast<int>(retrace::cli::ExitCode::trace_format_error),
      "validate maps a malformed later event to the trace-format error code");
  test.expect(validate_output.str().empty(),
              "a malformed event stream has no validation success output");
  test.expect(validate_error.str() == inspect_error.str(),
              "validate reports the same malformed-prefix count");
}

void test_truncated_trace_preserves_the_complete_inspection_prefix(TestContext& test) {
  TemporaryPath trace_path;
  test.expect(!trace_path.value().empty(),
              "a temporary truncated trace path is available");
  if (trace_path.value().empty() || !write_inspection_trace(test, trace_path.value())) {
    return;
  }

  std::error_code filesystem_error;
  const auto original_size =
      std::filesystem::file_size(trace_path.value(), filesystem_error);
  test.expect(!filesystem_error && original_size > 0U,
              "the complete trace size is available before truncation");
  if (filesystem_error || original_size == 0U) {
    return;
  }
  std::filesystem::resize_file(trace_path.value(), original_size - 1U,
                               filesystem_error);
  test.expect(!filesystem_error, "the synthetic final frame is truncated");
  if (filesystem_error) {
    return;
  }

  const std::array inspect_arguments{std::string_view{"inspect"},
                                     std::string_view{trace_path.value()}};
  std::ostringstream inspect_output;
  std::ostringstream inspect_error;
  const auto inspect_result =
      retrace::cli::run(inspect_arguments, inspect_output, inspect_error);

  test.expect(
      inspect_result == static_cast<int>(retrace::cli::ExitCode::trace_format_error),
      "inspect reports a truncated final frame");
  test.expect(inspect_output.str().find("process.start") != std::string::npos &&
                  inspect_output.str().find("stdout.chunk") != std::string::npos &&
                  inspect_output.str().find("unknown(60000)") != std::string::npos,
              "inspect renders every complete event before the truncated frame");
  test.expect(inspect_output.str().find("process.exit") == std::string::npos,
              "inspect does not render the incomplete final event");
  test.expect(inspect_output.str().ends_with("result=unknown status=incomplete\n"),
              "inspect marks the rendered prefix as incomplete");
  test.expect(inspect_error.str() ==
                  "error: trace is incomplete\n"
                  "cause: incomplete final event frame\n"
                  "complete events: 3\n",
              "inspect reports the exact number of complete prefix events");

  const std::array validate_arguments{std::string_view{"validate"},
                                      std::string_view{trace_path.value()}};
  std::ostringstream validate_output;
  std::ostringstream validate_error;
  const auto validate_result =
      retrace::cli::run(validate_arguments, validate_output, validate_error);

  test.expect(
      validate_result == static_cast<int>(retrace::cli::ExitCode::trace_format_error),
      "validate reports a truncated final frame");
  test.expect(validate_output.str().empty(),
              "an incomplete trace has no validation success output");
  test.expect(validate_error.str() == inspect_error.str(),
              "validate reports the same complete-event prefix count");
}

void test_run_requires_a_target_after_separator(TestContext& test) {
  constexpr std::array arguments{std::string_view{"run"}, std::string_view{"--"}};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::usage_error),
              "run without a target returns a usage error");
  test.expect(output.str().empty(), "invalid run syntax has no normal output");
  test.expect(error.str() ==
                  "usage: retrace run [--output TRACE] [--working-directory PATH] -- "
                  "COMMAND [ARGS...]\n",
              "invalid run syntax shows the exact command shape");
}

void test_run_help_describes_implemented_behavior(TestContext& test) {
  constexpr std::array arguments{std::string_view{"run"}, std::string_view{"--help"}};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::success),
              "run --help returns success");
  test.expect(output.str().starts_with("Usage:\n  retrace run") &&
                  output.str().find("--output TRACE") != std::string::npos &&
                  output.str().find("--working-directory PATH") != std::string::npos &&
                  output.str().find("SIGINT and SIGTERM") != std::string::npos,
              "run help describes every implemented run control");
  test.expect(error.str().empty(), "run --help has no error output");
}

void test_run_selects_and_records_working_directory(TestContext& test) {
  TemporaryDirectory directory;
  TemporaryPath trace_path;
  test.expect(!directory.value().empty() && !trace_path.value().empty(),
              "working-directory test paths are available");
  if (directory.value().empty() || trace_path.value().empty()) {
    return;
  }

  const std::array arguments{std::string_view{"run"},
                             std::string_view{"--working-directory"},
                             std::string_view{directory.value()},
                             std::string_view{"--output"},
                             std::string_view{trace_path.value()},
                             std::string_view{"--"},
                             std::string_view{"/bin/pwd"}};
  std::ostringstream output;
  std::ostringstream error;
  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::success),
              "run accepts a selected working directory");
  test.expect(output.str() == directory.value() + "\n",
              "the CLI target observes the selected working directory");
  test.expect(error.str().empty(),
              "a valid selected working directory has no diagnostic");

  retrace::trace::Reader reader;
  const auto open_error = retrace::trace::Reader::open(trace_path.value(), reader);
  test.expect(!open_error, "the working-directory trace can be opened");
  if (!open_error) {
    test.expect(reader.header().working_directory == directory.value(),
                "trace metadata records the target's selected directory");
  }
}

void test_run_rejects_a_missing_working_directory_before_trace_creation(
    TestContext& test) {
  TemporaryPath trace_path;
  test.expect(!trace_path.value().empty(),
              "a missing trace path is available for directory validation");
  if (trace_path.value().empty()) {
    return;
  }

  const std::array arguments{
      std::string_view{"run"},
      std::string_view{"--output"},
      std::string_view{trace_path.value()},
      std::string_view{"--working-directory"},
      std::string_view{"/definitely/not/a/retrace-working-directory"},
      std::string_view{"--"},
      std::string_view{"/bin/true"}};
  std::ostringstream output;
  std::ostringstream error;
  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::internal_error),
              "a missing selected directory returns an internal error");
  test.expect(output.str().empty(),
              "an invalid selected directory does not launch the target");
  test.expect(
      error.str().starts_with("error: working directory could not be selected\n") &&
          error.str().find("target was not started") != std::string::npos,
      "working-directory validation explains that the target did not run");
  test.expect(::access(trace_path.value().c_str(), F_OK) != 0,
              "working-directory validation happens before trace creation");
}

void test_run_preserves_target_exit_code(TestContext& test) {
  constexpr std::array arguments{std::string_view{"run"}, std::string_view{"--"},
                                 std::string_view{"/bin/sh"}, std::string_view{"-c"},
                                 std::string_view{"exit 7"}};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == 7, "run returns the target's non-zero exit code");
  test.expect(output.str().empty(), "run adds no output around the target");
  test.expect(error.str().empty(), "an ordinary target exit is not a RETRACE error");
}

void test_run_reports_a_missing_executable(TestContext& test) {
  constexpr std::array arguments{std::string_view{"run"}, std::string_view{"--"},
                                 std::string_view{"/definitely/not/a/retrace-command"}};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::target_launch_error),
              "a missing executable returns the launch-error code");
  test.expect(output.str().empty(), "a launch error has no normal output");
  test.expect(error.str().starts_with("error: target could not be started\n"),
              "a launch error identifies the failing stage");
  test.expect(error.str().find("/definitely/not") == std::string::npos,
              "a launch error does not repeat target arguments");
}

void test_run_routes_target_output(TestContext& test) {
  constexpr std::array arguments{
      std::string_view{"run"}, std::string_view{"--"}, std::string_view{"/bin/sh"},
      std::string_view{"-c"},
      std::string_view{"printf 'target output'; printf 'target error' >&2"}};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::success),
              "a target that writes both streams returns success");
  test.expect(output.str() == "target output",
              "run forwards captured stdout to normal CLI output");
  test.expect(error.str() == "target error",
              "run forwards captured stderr to CLI error output");
}

void test_recording_reports_incomplete_trace_after_output_failure(TestContext& test) {
  TemporaryPath trace_path;
  test.expect(!trace_path.value().empty(),
              "a temporary path is available for failed recorded output");
  if (trace_path.value().empty()) {
    return;
  }

  const std::array arguments{std::string_view{"run"},
                             std::string_view{"--output"},
                             std::string_view{trace_path.value()},
                             std::string_view{"--"},
                             std::string_view{"/bin/echo"},
                             std::string_view{"recorded output"}};
  SyncFailingStreamBuffer output_buffer;
  std::ostream output{&output_buffer};
  std::ostringstream error;

  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::internal_error),
              "a failed target-output destination is a supervisor error");
  test.expect(
      error.str().find("RETRACE could not supervise the target") != std::string::npos,
      "the output failure identifies process supervision");
  test.expect(
      error.str().find("note: the trace may be incomplete") != std::string::npos,
      "a finalized writer does not hide semantic trace incompleteness");
}

void test_run_records_a_runtime_handshake(TestContext& test) {
  TemporaryPath trace_path;
  test.expect(!trace_path.value().empty(),
              "a temporary runtime-handshake trace path is available");
  if (trace_path.value().empty()) {
    return;
  }

  const std::array arguments{std::string_view{"run"},
                             std::string_view{"--output"},
                             std::string_view{trace_path.value()},
                             std::string_view{"--"},
                             std::string_view{RETRACE_RUNTIME_CHANNEL_FIXTURE_PATH},
                             std::string_view{"load-runtime"}};
  std::ostringstream output;
  std::ostringstream error;
  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::success),
              "run succeeds when the target runtime handshakes");
  test.expect(output.str().empty() && error.str().empty(),
              "a runtime handshake does not contaminate target streams");

  const auto frames = read_trace_frames(trace_path.value());
  const auto handshake =
      std::find_if(frames.begin(), frames.end(), [](const auto& frame) {
        return frame.type ==
               static_cast<std::uint16_t>(retrace::trace::EventType::runtime_handshake);
      });
  test.expect(handshake != frames.end() && handshake->payload.empty(),
              "recording stores an empty runtime-handshake frame");

  const std::array inspect_arguments{std::string_view{"inspect"},
                                     std::string_view{trace_path.value()}};
  std::ostringstream inspect_output;
  std::ostringstream inspect_error;
  const auto inspect_result =
      retrace::cli::run(inspect_arguments, inspect_output, inspect_error);
  test.expect(inspect_result == static_cast<int>(retrace::cli::ExitCode::success) &&
                  inspect_output.str().find("runtime.handshake") != std::string::npos,
              "inspect renders runtime handshake evidence");
  test.expect(inspect_error.str().empty(),
              "a valid runtime handshake trace has no inspection diagnostic");
}

void test_run_writes_an_explicit_trace_without_overwriting(TestContext& test) {
  TemporaryPath trace_path;
  test.expect(!trace_path.value().empty(), "a temporary CLI trace path is available");
  const std::array arguments{std::string_view{"run"},
                             std::string_view{"--output"},
                             std::string_view{trace_path.value()},
                             std::string_view{"--"},
                             std::string_view{"/bin/sh"},
                             std::string_view{"-c"},
                             std::string_view{"printf 'recorded output'"}};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::success),
              "run succeeds while writing an explicit trace");
  test.expect(output.str() == "recorded output",
              "recording still forwards target output");
  test.expect(error.str().empty(), "a successful recording has no diagnostic");

  const auto frames = read_trace_frames(trace_path.value());
  test.expect(frames.size() == 4U,
              "recording writes start, exec, stdout, and exit frames");
  if (frames.size() == 4U) {
    test.expect(frames[0].type == static_cast<std::uint16_t>(
                                      retrace::trace::EventType::process_start),
                "the recording starts with a process-start frame");
    test.expect(frames[1].type ==
                    static_cast<std::uint16_t>(retrace::trace::EventType::process_exec),
                "the recording confirms successful exec");
    test.expect(frames[2].type == static_cast<std::uint16_t>(
                                      retrace::trace::EventType::standard_output) &&
                    frames[2].payload == "recorded output",
                "the recording stores the captured stdout bytes");
    test.expect(frames[3].type == static_cast<std::uint16_t>(
                                      retrace::trace::EventType::process_exit) &&
                    frames[3].payload.size() == sizeof(std::uint32_t) &&
                    static_cast<unsigned char>(frames[3].payload[0]) == 0U,
                "the recording ends with exit status zero");
  }

  std::ostringstream duplicate_output;
  std::ostringstream duplicate_error;
  const auto duplicate_result =
      retrace::cli::run(arguments, duplicate_output, duplicate_error);
  test.expect(
      duplicate_result == static_cast<int>(retrace::cli::ExitCode::internal_error),
      "run refuses an existing output path");
  test.expect(duplicate_output.str().empty(),
              "a refused trace does not launch the target");
  test.expect(
      duplicate_error.str().find("trace could not be created") != std::string::npos,
      "an existing trace produces a trace-creation diagnostic");
  test.expect(duplicate_error.str().find("target was not started") != std::string::npos,
              "the diagnostic states that the target did not run");
}

}  // namespace

int main() {
  TestContext test;
  test_empty_arguments_show_help(test);
  test_version(test);
  test_unknown_command(test);
  test_version_rejects_extra_arguments(test);
  test_run_records_a_forwarded_signal(test);
  test_trace_commands_require_exactly_one_path(test);
  test_trace_commands_report_a_missing_file(test);
  test_validate_accepts_a_structurally_valid_trace(test);
  test_inspect_renders_safe_metadata_and_timeline(test);
  test_validate_reports_a_normal_output_failure(test);
  test_inspect_reports_a_normal_output_failure(test);
  test_trace_commands_reject_a_malformed_header(test);
  test_trace_commands_reject_an_unsupported_version(test);
  test_trace_commands_reject_a_non_regular_source(test);
  test_malformed_event_reports_the_complete_prefix(test);
  test_truncated_trace_preserves_the_complete_inspection_prefix(test);
  test_run_requires_a_target_after_separator(test);
  test_run_help_describes_implemented_behavior(test);
  test_run_selects_and_records_working_directory(test);
  test_run_rejects_a_missing_working_directory_before_trace_creation(test);
  test_run_preserves_target_exit_code(test);
  test_run_reports_a_missing_executable(test);
  test_run_routes_target_output(test);
  test_recording_reports_incomplete_trace_after_output_failure(test);
  test_run_records_a_runtime_handshake(test);
  test_run_writes_an_explicit_trace_without_overwriting(test);
  return test.result();
}
