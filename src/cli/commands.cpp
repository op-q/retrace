// Implements the user-facing command router and `run` workflow: parse options,
// optionally open a trace, supervise the target, and map outcomes to CLI codes.

#include <sys/utsname.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "retrace/cli.hpp"
#include "retrace/process.hpp"
#include "retrace/trace.hpp"
#include "retrace/version.hpp"
#include "trace_commands.hpp"

#ifndef RETRACE_BUILD_EXECUTABLE_DIRECTORY
#error "RETRACE_BUILD_EXECUTABLE_DIRECTORY must name the build binary directory"
#endif

#ifndef RETRACE_BUILD_RUNTIME_LIBRARY_PATH
#error "RETRACE_BUILD_RUNTIME_LIBRARY_PATH must name the build runtime library"
#endif

#ifndef RETRACE_INSTALL_LIBDIR_FROM_BINDIR
#error "RETRACE_INSTALL_LIBDIR_FROM_BINDIR must locate the installed library directory"
#endif

namespace retrace::cli {
namespace {

constexpr std::string_view help_text = R"(Usage: retrace COMMAND [OPTIONS]

Commands:
  run      Launch a command, optionally record it, and return its exit status
  inspect  Render a recorded trace
  validate Validate a trace file
  version  Print the RETRACE version
  help     Explain how to use RETRACE
)";

constexpr std::string_view run_usage =
    "usage: retrace run [--output TRACE] [--working-directory PATH] "
    "[--no-runtime] -- "
    "COMMAND [ARGS...]\n";

constexpr std::string_view run_help_text = R"(Usage:
  retrace run [--output TRACE] [--working-directory PATH] [--no-runtime] --
    COMMAND [ARGS...]

Options:
  --output TRACE             Record the run without overwriting TRACE
  --working-directory PATH   Run the target from an existing directory
  --no-runtime               Do not load the RETRACE runtime

Signals:
  SIGINT and SIGTERM received by RETRACE are forwarded to the target group.
)";

[[nodiscard]] bool is_help(const std::string_view argument) {
  return argument == "help" || argument == "--help" || argument == "-h";
}

[[nodiscard]] bool is_version(const std::string_view argument) {
  return argument == "version" || argument == "--version" || argument == "-V";
}

[[nodiscard]] std::error_code forward_output(std::ostream& destination,
                                             const std::string_view bytes) noexcept {
  try {
    destination.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    destination.flush();
    if (!destination) {
      return std::make_error_code(std::errc::io_error);
    }
  } catch (const std::ios_base::failure& error) {
    return error.code();
  } catch (...) {
    return std::make_error_code(std::errc::io_error);
  }
  return {};
}

// Translates one observed libc operation into its on-disk event. The live
// protocol and the trace format are separate contracts, so each field is copied
// explicitly rather than reinterpreted: only the members meaningful for the
// operation are stored, and the rest stay zero.
[[nodiscard]] std::error_code record_operation(trace::Writer& writer,
                                               const process::ProcessEvent& event) {
  const auto* const operation = event.operation;
  if (operation == nullptr) {
    return std::make_error_code(std::errc::protocol_error);
  }
  const auto process_id = static_cast<std::uint32_t>(event.process_id);

  trace::FileEvent fields{
      .completion_offset_nanoseconds =
          writer.offset_from_monotonic(operation->monotonic_nanoseconds),
      .duration_nanoseconds = operation->duration_nanoseconds,
      .result = operation->result,
      .error_number = operation->error_number,
      .descriptor = operation->descriptor,
      // Explicit defaults for the remaining members prevent GCC's missing-field
      // warning and keep every operation-specific field visible at this site.
      .directory = 0,
      .open_flags = 0U,
      .mode = 0U,
      .flags = 0U,
      .path = {},
  };

  switch (operation->kind) {
    case process::RuntimeOperationKind::file_openat:
      // Only openat resolves against a directory descriptor. Plain open reports
      // AT_FDCWD, which would be a meaningless value to store as a base.
      fields.flags |= trace::file_event_flag_relative_to_directory;
      fields.directory = operation->directory;
      [[fallthrough]];
    case process::RuntimeOperationKind::file_open:
      fields.open_flags = operation->open_flags;
      fields.mode = operation->mode;
      fields.path = operation->path;
      if (operation->path_truncated) {
        fields.flags |= trace::file_event_flag_path_truncated;
      }
      return writer.write_file_event(trace::EventType::file_open, process_id,
                                     operation->thread_id, fields);
    case process::RuntimeOperationKind::file_close:
      return writer.write_file_event(trace::EventType::file_close, process_id,
                                     operation->thread_id, fields);
  }

  return std::make_error_code(std::errc::protocol_error);
}

[[nodiscard]] std::error_code record_event(trace::Writer& writer,
                                           const process::ProcessEvent& event) {
  // Process events are an in-memory contract. This switch is the explicit
  // translation boundary into stable on-disk trace event IDs and payloads.
  if (event.process_id <= 0) {
    return std::make_error_code(std::errc::protocol_error);
  }
  if ((event.type == process::ProcessEventType::exited ||
       event.type == process::ProcessEventType::signaled ||
       event.type == process::ProcessEventType::signal_forwarded ||
       event.type == process::ProcessEventType::launch_failed) &&
      event.value < 0) {
    return std::make_error_code(std::errc::protocol_error);
  }
  const auto process_id = static_cast<std::uint32_t>(event.process_id);

  switch (event.type) {
    case process::ProcessEventType::started:
      return writer.write_event(trace::EventType::process_start, process_id);
    case process::ProcessEventType::exec_succeeded:
      return writer.write_event(trace::EventType::process_exec, process_id);
    case process::ProcessEventType::runtime_handshake:
      return writer.write_event(trace::EventType::runtime_handshake, process_id);
    case process::ProcessEventType::runtime_operation:
      return record_operation(writer, event);
    case process::ProcessEventType::standard_output:
      return writer.write_event(trace::EventType::standard_output, process_id,
                                event.bytes);
    case process::ProcessEventType::standard_error:
      return writer.write_event(trace::EventType::standard_error, process_id,
                                event.bytes);
    case process::ProcessEventType::signal_forwarded:
      return writer.write_value_event(trace::EventType::signal_receive, process_id,
                                      static_cast<std::uint32_t>(event.value));
    case process::ProcessEventType::exited:
      return writer.write_value_event(trace::EventType::process_exit, process_id,
                                      static_cast<std::uint32_t>(event.value));
    case process::ProcessEventType::signaled:
      return writer.write_value_event(trace::EventType::process_signal, process_id,
                                      static_cast<std::uint32_t>(event.value));
    case process::ProcessEventType::launch_failed:
      return writer.write_value_event(trace::EventType::process_launch_failure,
                                      process_id,
                                      static_cast<std::uint32_t>(event.value));
  }

  return std::make_error_code(std::errc::protocol_error);
}

struct RunArguments {
  std::optional<std::string_view> output_path;
  std::optional<std::string_view> working_directory;
  std::span<const std::string_view> target;
  bool no_runtime = false;
};

[[nodiscard]] bool parse_run_arguments(
    const std::span<const std::string_view> arguments, RunArguments& result) {
  // Parsing stays dependency-free while the option surface is small. Each
  // path option consumes one following value; flags advance independently.
  std::size_t index = 1U;
  while (index < arguments.size() && arguments[index] != "--") {
    if (arguments[index] == "--no-runtime") {
      if (result.no_runtime) {
        return false;
      }
      result.no_runtime = true;
      ++index;
      continue;
    }
    if (index + 1U >= arguments.size()) {
      return false;
    }
    if (arguments[index] == "--output" && !result.output_path) {
      result.output_path = arguments[index + 1U];
    } else if (arguments[index] == "--working-directory" && !result.working_directory) {
      result.working_directory = arguments[index + 1U];
    } else {
      return false;
    }
    index += 2U;
  }

  if (index >= arguments.size() || arguments[index] != "--" ||
      index + 1U >= arguments.size()) {
    return false;
  }
  result.target = arguments.subspan(index + 1U);
  return true;
}

[[nodiscard]] std::error_code locate_runtime_library(std::filesystem::path& result) {
  // Build binaries use the exact target path produced by CMake. Installed
  // binaries derive the configured libdir from /proc/self/exe, so a stale local
  // build cannot accidentally become an installed command's preload source.
  std::error_code executable_error;
  const auto executable =
      std::filesystem::canonical("/proc/self/exe", executable_error);
  if (executable_error) {
    return executable_error;
  }

  std::error_code build_directory_error;
  const auto build_directory = std::filesystem::canonical(
      std::filesystem::path{RETRACE_BUILD_EXECUTABLE_DIRECTORY}, build_directory_error);

  const std::filesystem::path build_runtime{RETRACE_BUILD_RUNTIME_LIBRARY_PATH};
  const auto candidate =
      !build_directory_error && executable.parent_path() == build_directory
          ? build_runtime
          : executable.parent_path() /
                std::filesystem::path{RETRACE_INSTALL_LIBDIR_FROM_BINDIR} /
                build_runtime.filename();

  std::error_code runtime_error;
  auto runtime = std::filesystem::canonical(candidate, runtime_error);
  if (runtime_error) {
    return runtime_error;
  }
  if (!std::filesystem::is_regular_file(runtime, runtime_error)) {
    return runtime_error ? runtime_error
                         : std::make_error_code(std::errc::no_such_file_or_directory);
  }
  const auto& bytes = runtime.native();
  if (bytes.empty() || std::any_of(bytes.begin(), bytes.end(), [](const char byte) {
        return byte == ':' || byte == ' ' || byte == '\t' || byte == '\n' ||
               byte == '\r' || byte == '\f' || byte == '\v';
      })) {
    return std::make_error_code(std::errc::invalid_argument);
  }

  result = std::move(runtime);
  return {};
}

[[nodiscard]] trace::CreationResult create_trace_writer(
    const std::string_view output_path,
    const std::span<const std::string_view> target_arguments,
    const std::filesystem::path& working_directory, trace::Writer& writer) {
  // System identity and the canonical working directory become immutable trace
  // metadata before the target starts.
  utsname system_information{};
  if (::uname(&system_information) < 0) {
    return {.error = {errno, std::generic_category()}};
  }

  const auto& working_directory_bytes = working_directory.native();
  const trace::Metadata metadata{
      .retrace_version = retrace::version,
      .operating_system = system_information.sysname,
      .architecture = system_information.machine,
      .working_directory = working_directory_bytes,
      .arguments = target_arguments,
  };
  return trace::Writer::create(std::filesystem::path{std::string{output_path}},
                               metadata, writer);
}

[[nodiscard]] int run_target(const std::span<const std::string_view> arguments,
                             std::ostream& output, std::ostream& error) {
  // Setup errors occur before process::execute so diagnostics can truthfully say
  // whether the target was ever started.
  if (arguments.size() == 2U && is_help(arguments[1])) {
    output << run_help_text;
    return static_cast<int>(ExitCode::success);
  }

  RunArguments run_arguments;
  if (!parse_run_arguments(arguments, run_arguments)) {
    error << run_usage;
    return static_cast<int>(ExitCode::usage_error);
  }

  std::error_code working_directory_error;
  std::filesystem::path working_directory;
  if (run_arguments.working_directory) {
    if (run_arguments.working_directory->empty() ||
        run_arguments.working_directory->find('\0') != std::string_view::npos) {
      working_directory_error = std::make_error_code(std::errc::invalid_argument);
    } else {
      working_directory = std::filesystem::canonical(
          std::filesystem::path{std::string{*run_arguments.working_directory}},
          working_directory_error);
      if (!working_directory_error &&
          !std::filesystem::is_directory(working_directory, working_directory_error)) {
        working_directory_error = std::make_error_code(std::errc::not_a_directory);
      }
    }
  } else {
    working_directory = std::filesystem::current_path(working_directory_error);
  }
  if (working_directory_error) {
    error << "error: working directory could not be selected\n"
          << "cause: " << working_directory_error.message() << '\n'
          << "target was not started\n";
    return static_cast<int>(ExitCode::internal_error);
  }

  std::filesystem::path runtime_library;
  if (!run_arguments.no_runtime) {
    if (const auto runtime_error = locate_runtime_library(runtime_library)) {
      error << "error: RETRACE runtime library could not be selected\n"
            << "cause: " << runtime_error.message() << '\n'
            << "target was not started\n";
      return static_cast<int>(ExitCode::internal_error);
    }
  }

  trace::Writer trace_writer;
  if (run_arguments.output_path) {
    const auto creation =
        create_trace_writer(*run_arguments.output_path, run_arguments.target,
                            working_directory, trace_writer);
    if (creation.error) {
      error << "error: trace could not be created\n"
            << "cause: " << creation.error.message() << '\n'
            << "target was not started\n";
      if (creation.file_created) {
        error << "note: an incomplete trace file exists\n";
      }
      return static_cast<int>(ExitCode::internal_error);
    }
  }

  std::error_code trace_error;
  const auto& working_directory_bytes = working_directory.native();
  const auto& runtime_library_bytes = runtime_library.native();
  const auto result = process::execute(
      run_arguments.target,
      [&output, &error, &trace_writer,
       &trace_error](const process::ProcessEvent& event) {
        if (trace_writer.is_open() && !trace_error) {
          trace_error = record_event(trace_writer, event);
        }
        if (event.type != process::ProcessEventType::standard_output &&
            event.type != process::ProcessEventType::standard_error) {
          return std::error_code{};
        }
        auto& destination =
            event.type == process::ProcessEventType::standard_output ? output : error;
        return forward_output(destination, event.bytes);
      },
      {.working_directory = working_directory_bytes,
       .runtime_library = runtime_library_bytes,
       .runtime_channel_enabled = !run_arguments.no_runtime});

  // The drop count is only final once supervision ends, so it is recorded as a
  // closing event. A trace that silently omitted operations would otherwise look
  // exactly like a complete one.
  if (trace_writer.is_open() && !trace_error &&
      result.dropped_runtime_operations > 0U && result.process_id > 0) {
    trace_error = trace_writer.write_dropped_operations_event(
        static_cast<std::uint32_t>(result.process_id),
        result.dropped_runtime_operations);
  }

  if (trace_writer.is_open()) {
    const auto finish_error = trace_writer.finish();
    if (!trace_error) {
      trace_error = finish_error;
    }
  }

  if (result.dropped_runtime_operations > 0U) {
    error << "warning: the runtime channel dropped "
          << result.dropped_runtime_operations << " reported operations\n"
          << "cause: the bounded event channel could not keep up with the target\n";
  }

  if (trace_error) {
    error << "error: RETRACE could not complete the trace\n"
          << "cause: " << trace_error.message() << '\n'
          << "note: a partial trace may exist\n";
    return static_cast<int>(ExitCode::internal_error);
  }

  switch (result.state) {
    case process::ProcessState::exited:
      return result.exit_code;
    case process::ProcessState::signaled:
      error << "target terminated by signal " << result.signal_number << '\n';
      return 128 + result.signal_number;
    case process::ProcessState::launch_failed:
      error << "error: target could not be started\n"
            << "cause: " << result.error.message() << '\n';
      return static_cast<int>(ExitCode::target_launch_error);
    case process::ProcessState::runtime_unavailable:
      error << "error: RETRACE runtime could not instrument the target\n"
            << "cause: the required runtime handshake was not received\n";
      if (result.signal_number > 0) {
        error << "target result: signal(" << result.signal_number << ")\n";
      } else {
        error << "target result: exit(" << result.exit_code << ")\n";
      }
      if (run_arguments.output_path) {
        error << "note: the trace contains lifecycle data but no runtime handshake\n";
      }
      return static_cast<int>(ExitCode::internal_error);
    case process::ProcessState::supervisor_failed:
      error << "error: RETRACE could not supervise the target\n"
            << "cause: " << result.error.message() << '\n';
      if (run_arguments.output_path) {
        error << "note: the trace may be incomplete\n";
      }
      return static_cast<int>(ExitCode::internal_error);
  }

  error << "error: RETRACE received an unknown process state\n";
  return static_cast<int>(ExitCode::internal_error);
}

}  // namespace

int run(const std::span<const std::string_view> arguments, std::ostream& output,
        std::ostream& error) {
  if (arguments.empty() || is_help(arguments.front())) {
    output << help_text;
    return static_cast<int>(ExitCode::success);
  }

  if (is_version(arguments.front())) {
    if (arguments.size() != 1U) {
      error << "error: the version command does not accept arguments\n";
      return static_cast<int>(ExitCode::usage_error);
    }

    output << "retrace " << retrace::version << '\n';
    return static_cast<int>(ExitCode::success);
  }

  if (arguments.front() == "run") {
    return run_target(arguments, output, error);
  }
  if (arguments.front() == "inspect") {
    return inspect_trace(arguments, output, error);
  }
  if (arguments.front() == "validate") {
    return validate_trace(arguments, output, error);
  }

  error << "error: unknown command '" << arguments.front() << "'\n"
        << "try 'retrace help' for usage\n";
  return static_cast<int>(ExitCode::usage_error);
}

}  // namespace retrace::cli
