// Implements safe terminal rendering and structural validation for trace files.
// All trace-controlled bytes are escaped and bounded before reaching a terminal.

#include "trace_commands.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <ios>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include "retrace/cli.hpp"
#include "retrace/trace.hpp"

namespace retrace::cli {
namespace {

constexpr std::size_t metadata_preview_size = 160U;
constexpr std::size_t stream_preview_size = 64U;
constexpr std::size_t displayed_argument_count = 16U;

[[nodiscard]] std::uint32_t decode_u32(const std::string_view bytes) {
  std::uint32_t result = 0U;
  for (std::size_t index = 0; index < sizeof(result); ++index) {
    result |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[index]))
              << (index * 8U);
  }
  return result;
}

[[nodiscard]] std::uint64_t decode_u64(const std::string_view bytes) {
  std::uint64_t result = 0U;
  for (std::size_t index = 0; index < sizeof(result); ++index) {
    result |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[index]))
              << (index * 8U);
  }
  return result;
}

// Numeric bases are rendered through a local stream so the caller's formatting
// state is never modified. A leaked std::hex would silently reformat every
// later field on the timeline.
[[nodiscard]] std::string formatted_hexadecimal(const std::uint32_t value) {
  std::ostringstream result;
  result << "0x" << std::hex << value;
  return result.str();
}

[[nodiscard]] std::string formatted_octal(const std::uint32_t value) {
  std::ostringstream result;
  result << '0' << std::oct << value;
  return result.str();
}

[[nodiscard]] std::string escaped(const std::string_view bytes,
                                  const std::size_t maximum_bytes) {
  // Escaping is a security boundary: raw trace bytes may contain terminal
  // control sequences, invalid text, embedded NULs, or very large payloads.
  constexpr std::string_view hexadecimal = "0123456789abcdef";
  const auto preview_size = std::min(bytes.size(), maximum_bytes);
  std::string result;
  result.reserve(preview_size + 2U);
  result.push_back('"');
  for (std::size_t index = 0; index < preview_size; ++index) {
    const auto byte = static_cast<unsigned char>(bytes[index]);
    switch (byte) {
      case '\0':
        result += "\\0";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      default:
        if (byte >= 0x20U && byte <= 0x7eU) {
          result.push_back(static_cast<char>(byte));
        } else {
          result += "\\x";
          result.push_back(hexadecimal[byte >> 4U]);
          result.push_back(hexadecimal[byte & 0x0fU]);
        }
        break;
    }
  }
  if (preview_size < bytes.size()) {
    result += "...";
  }
  result.push_back('"');
  return result;
}

[[nodiscard]] std::string formatted_offset(const std::uint64_t nanoseconds) {
  constexpr std::uint64_t nanoseconds_per_millisecond = 1000000U;
  constexpr std::uint64_t nanoseconds_per_microsecond = 1000U;
  const auto milliseconds = nanoseconds / nanoseconds_per_millisecond;
  const auto fractional_microseconds =
      (nanoseconds % nanoseconds_per_millisecond) / nanoseconds_per_microsecond;

  std::ostringstream result;
  result << milliseconds << '.' << std::setfill('0') << std::setw(3)
         << fractional_microseconds << " ms";
  return result.str();
}

[[nodiscard]] std::string_view event_name(const std::uint16_t type) {
  switch (static_cast<trace::EventType>(type)) {
    case trace::EventType::process_start:
      return "process.start";
    case trace::EventType::process_exec:
      return "process.exec";
    case trace::EventType::standard_output:
      return "stdout.chunk";
    case trace::EventType::standard_error:
      return "stderr.chunk";
    case trace::EventType::process_exit:
      return "process.exit";
    case trace::EventType::process_signal:
      return "process.signal";
    case trace::EventType::process_launch_failure:
      return "process.launch_failure";
    case trace::EventType::signal_receive:
      return "signal.receive";
    case trace::EventType::runtime_handshake:
      return "runtime.handshake";
    case trace::EventType::file_open:
      return "file.open";
    case trace::EventType::file_close:
      return "file.close";
    case trace::EventType::runtime_operations_dropped:
      return "runtime.operations_dropped";
  }
  return {};
}

// Renders one decoded file event. Only the fields meaningful for the operation
// are printed: a failed open has no descriptor to report, and a close names the
// descriptor it was given whether or not it succeeded.
void render_file_event(const trace::EventType type, const trace::FileEvent& file,
                       std::ostream& output) {
  const bool failed = file.result < 0;
  if (type == trace::EventType::file_close) {
    output << " fd=" << file.descriptor;
    if (failed) {
      output << " errno=" << file.error_number;
    }
  } else if (failed) {
    output << " errno=" << file.error_number;
  } else {
    output << " fd=" << file.descriptor;
  }

  if (type == trace::EventType::file_open) {
    if ((file.flags & trace::file_event_flag_relative_to_directory) != 0U) {
      output << " dirfd=" << file.directory;
    }
    output << " path=" << escaped(file.path, metadata_preview_size);
    if ((file.flags & trace::file_event_flag_path_truncated) != 0U) {
      output << " (path truncated)";
    }
    output << " open_flags=" << formatted_hexadecimal(file.open_flags);
    if (file.mode != 0U) {
      output << " mode=" << formatted_octal(file.mode);
    }
  }

  // The leading column is the supervisor's receive time. `completed` is the
  // target's own completion time, which is what orders concurrent operations.
  output << " duration=" << formatted_offset(file.duration_nanoseconds)
         << " completed=" << formatted_offset(file.completion_offset_nanoseconds);
}

void render_header(const trace::Header& header, std::ostream& output) {
  output << "TRACE version=" << header.major_version << '.' << header.minor_version
         << '\n'
         << "RECORDER " << escaped(header.retrace_version, metadata_preview_size)
         << '\n'
         << "SYSTEM " << escaped(header.operating_system, metadata_preview_size)
         << " architecture=" << escaped(header.architecture, metadata_preview_size)
         << '\n'
         << "COMMAND";

  const auto argument_count =
      std::min(header.arguments.size(), displayed_argument_count);
  for (std::size_t index = 0; index < argument_count; ++index) {
    output << ' ' << escaped(header.arguments[index], metadata_preview_size);
  }
  if (argument_count < header.arguments.size()) {
    output << " ... (" << header.arguments.size() - argument_count << " more)";
  }
  output << '\n'
         << "WORKING_DIRECTORY "
         << escaped(header.working_directory, metadata_preview_size) << '\n'
         << "CREATED_UNIX_NS " << header.realtime_base_nanoseconds << '\n'
         << "EVENTS\n";
}

enum class ResultKind : std::uint8_t {
  unknown,
  exited,
  signaled,
  launch_failed,
};

struct InspectionSummary {
  std::size_t event_count = 0U;
  std::uint64_t duration_nanoseconds = 0U;
  ResultKind result = ResultKind::unknown;
  std::uint32_t result_value = 0U;
  // Operations the runtime reported that never reached the supervisor. A
  // nonzero value means the timeline above is incomplete.
  std::uint64_t dropped_operations = 0U;
};

void render_event(const trace::Event& event, std::ostream& output,
                  InspectionSummary& summary) {
  // Known events receive semantic fields; unknown IDs keep an opaque, escaped
  // preview so forward-compatible traces remain understandable.
  ++summary.event_count;
  summary.duration_nanoseconds = event.offset_nanoseconds;

  const auto name = event_name(event.type);
  output << std::setw(15) << formatted_offset(event.offset_nanoseconds) << "  ";
  if (name.empty()) {
    output << "unknown(" << event.type << ')';
  } else {
    output << std::left << std::setw(23) << name << std::right;
  }
  output << " pid=" << event.process_id;

  if (event.type == static_cast<std::uint16_t>(trace::EventType::standard_output) ||
      event.type == static_cast<std::uint16_t>(trace::EventType::standard_error)) {
    output << " bytes=" << event.payload.size()
           << " preview=" << escaped(event.payload, stream_preview_size);
  } else if (event.type == static_cast<std::uint16_t>(trace::EventType::process_exit)) {
    summary.result = ResultKind::exited;
    summary.result_value = decode_u32(event.payload);
    output << " code=" << summary.result_value;
  } else if (event.type ==
             static_cast<std::uint16_t>(trace::EventType::process_signal)) {
    summary.result = ResultKind::signaled;
    summary.result_value = decode_u32(event.payload);
    output << " signal=" << summary.result_value;
  } else if (event.type ==
             static_cast<std::uint16_t>(trace::EventType::process_launch_failure)) {
    summary.result = ResultKind::launch_failed;
    summary.result_value = decode_u32(event.payload);
    output << " errno=" << summary.result_value;
  } else if (event.type ==
             static_cast<std::uint16_t>(trace::EventType::signal_receive)) {
    output << " signal=" << decode_u32(event.payload);
  } else if (event.type == static_cast<std::uint16_t>(trace::EventType::file_open) ||
             event.type == static_cast<std::uint16_t>(trace::EventType::file_close)) {
    const auto type = static_cast<trace::EventType>(event.type);
    trace::FileEvent file;
    // Reader validation already rejected an undecodable payload, so this only
    // guards the rendering path against ever printing uninitialized fields.
    if (trace::decode_file_event(type, event.payload, file)) {
      output << " tid=" << event.thread_id;
      render_file_event(type, file, output);
    }
  } else if (event.type ==
             static_cast<std::uint16_t>(trace::EventType::runtime_operations_dropped)) {
    summary.dropped_operations = decode_u64(event.payload);
    output << " count=" << summary.dropped_operations;
  } else if (name.empty()) {
    output << " tid=" << event.thread_id << " bytes=" << event.payload.size()
           << " preview=" << escaped(event.payload, stream_preview_size);
  }
  output << '\n';
}

void render_summary(const InspectionSummary& summary, const std::string_view status,
                    std::ostream& output) {
  output << "SUMMARY events=" << summary.event_count
         << " duration=" << formatted_offset(summary.duration_nanoseconds)
         << " result=";
  switch (summary.result) {
    case ResultKind::unknown:
      output << "unknown";
      break;
    case ResultKind::exited:
      output << "exit(" << summary.result_value << ')';
      break;
    case ResultKind::signaled:
      output << "signal(" << summary.result_value << ')';
      break;
    case ResultKind::launch_failed:
      output << "launch-failure(" << summary.result_value << ')';
      break;
  }
  if (summary.dropped_operations > 0U) {
    output << " dropped_operations=" << summary.dropped_operations;
  }
  output << " status=" << status << '\n';
}

[[nodiscard]] int report_open_error(const std::error_code& read_error,
                                    std::ostream& error) {
  if (trace::is_trace_error(read_error)) {
    error << "error: invalid trace\n"
          << "cause: " << read_error.message() << '\n';
    return static_cast<int>(ExitCode::trace_format_error);
  }
  error << "error: trace could not be opened\n"
        << "cause: " << read_error.message() << '\n';
  return static_cast<int>(ExitCode::internal_error);
}

[[nodiscard]] int report_read_error(const std::error_code& read_error,
                                    const std::size_t complete_events,
                                    std::ostream& error) {
  if (trace::is_trace_error(read_error)) {
    error << "error: trace is malformed\n";
  } else {
    error << "error: trace could not be read\n";
  }
  error << "cause: " << read_error.message() << '\n'
        << "complete events: " << complete_events << '\n';
  return trace::is_trace_error(read_error)
             ? static_cast<int>(ExitCode::trace_format_error)
             : static_cast<int>(ExitCode::internal_error);
}

[[nodiscard]] int report_output_error(std::ostream& error) {
  error << "error: command output could not be written\n"
        << "cause: normal output stream failure\n";
  return static_cast<int>(ExitCode::internal_error);
}

template <typename Render>
[[nodiscard]] bool write_output(std::ostream& output, Render&& render,
                                const bool flush = false) {
  try {
    render();
    if (flush) {
      output.flush();
    }
  } catch (const std::ios_base::failure&) {
    return false;
  } catch (...) {
    return false;
  }
  return output.good();
}

}  // namespace

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
int inspect_trace(const std::span<const std::string_view> arguments,
                  std::ostream& output, std::ostream& error) {
  if (arguments.size() != 2U) {
    error << "usage: retrace inspect TRACE\n";
    return static_cast<int>(ExitCode::usage_error);
  }

  trace::Reader reader;
  if (const auto read_error = trace::Reader::open(
          std::filesystem::path{std::string{arguments[1]}}, reader)) {
    return report_open_error(read_error, error);
  }

  if (!write_output(output,
                    [&reader, &output] { render_header(reader.header(), output); })) {
    return report_output_error(error);
  }
  InspectionSummary summary;
  while (true) {
    trace::Event event;
    const auto read = reader.next(event);
    if (read.status == trace::ReadStatus::event) {
      if (!write_output(output, [&event, &output, &summary] {
            render_event(event, output, summary);
          })) {
        return report_output_error(error);
      }
      continue;
    }
    if (read.status == trace::ReadStatus::end) {
      if (!write_output(
              output,
              [&output, &summary] {
                render_summary(summary, "structurally-valid", output);
              },
              true)) {
        return report_output_error(error);
      }
      return static_cast<int>(ExitCode::success);
    }
    if (read.status == trace::ReadStatus::incomplete_final_frame) {
      if (!write_output(
              output,
              [&output, &summary] { render_summary(summary, "incomplete", output); },
              true)) {
        return report_output_error(error);
      }
      error << "error: trace is incomplete\n"
            << "cause: incomplete final event frame\n"
            << "complete events: " << summary.event_count << '\n';
      return static_cast<int>(ExitCode::trace_format_error);
    }
    if (!write_output(output, [] {}, true)) {
      return report_output_error(error);
    }
    return report_read_error(read.error, summary.event_count, error);
  }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

int validate_trace(const std::span<const std::string_view> arguments,
                   std::ostream& output, std::ostream& error) {
  if (arguments.size() != 2U) {
    error << "usage: retrace validate TRACE\n";
    return static_cast<int>(ExitCode::usage_error);
  }

  trace::Reader reader;
  if (const auto read_error = trace::Reader::open(
          std::filesystem::path{std::string{arguments[1]}}, reader)) {
    return report_open_error(read_error, error);
  }

  std::size_t event_count = 0U;
  while (true) {
    trace::Event event;
    const auto read = reader.next(event);
    if (read.status == trace::ReadStatus::event) {
      ++event_count;
      continue;
    }
    if (read.status == trace::ReadStatus::end) {
      if (!write_output(output, [&output] { output << "trace is valid\n"; }, true)) {
        return report_output_error(error);
      }
      return static_cast<int>(ExitCode::success);
    }
    if (read.status == trace::ReadStatus::incomplete_final_frame) {
      error << "error: trace is incomplete\n"
            << "cause: incomplete final event frame\n"
            << "complete events: " << event_count << '\n';
      return static_cast<int>(ExitCode::trace_format_error);
    }
    return report_read_error(read.error, event_count, error);
  }
}

}  // namespace retrace::cli
