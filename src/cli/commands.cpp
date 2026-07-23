#include <sys/utsname.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>

#include "retrace/cli.hpp"
#include "retrace/process.hpp"
#include "retrace/trace.hpp"
#include "retrace/version.hpp"
#include "trace_commands.hpp"

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
    "usage: retrace run [--output TRACE] [--working-directory PATH] -- "
    "COMMAND [ARGS...]\n";

constexpr std::string_view run_help_text = R"(Usage:
  retrace run [--output TRACE] [--working-directory PATH] -- COMMAND [ARGS...]

Options:
  --output TRACE             Record the run without overwriting TRACE
  --working-directory PATH   Run the target from an existing directory

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

[[nodiscard]] std::error_code record_event(trace::Writer& writer,
                                           const process::ProcessEvent& event) {
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
};

[[nodiscard]] bool parse_run_arguments(
    const std::span<const std::string_view> arguments, RunArguments& result) {
  std::size_t index = 1U;
  while (index < arguments.size() && arguments[index] != "--") {
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

[[nodiscard]] trace::CreationResult create_trace_writer(
    const std::string_view output_path,
    const std::span<const std::string_view> target_arguments,
    const std::filesystem::path& working_directory, trace::Writer& writer) {
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
      {.working_directory = working_directory_bytes});

  if (trace_writer.is_open()) {
    const auto finish_error = trace_writer.finish();
    if (!trace_error) {
      trace_error = finish_error;
    }
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
