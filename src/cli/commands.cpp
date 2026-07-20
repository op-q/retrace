#include <ostream>

#include "retrace/cli.hpp"
#include "retrace/process.hpp"
#include "retrace/version.hpp"

namespace retrace::cli {
namespace {

constexpr std::string_view help_text = R"(Usage: retrace COMMAND [OPTIONS]

Commands:
  run      Launch a command and return its exit status
  version  Print the RETRACE version
  help     Explain how to use RETRACE

Planned for v0.1:
  inspect  Render a recorded trace
  validate Validate a trace file
)";

[[nodiscard]] bool is_help(const std::string_view argument) {
  return argument == "help" || argument == "--help" || argument == "-h";
}

[[nodiscard]] bool is_version(const std::string_view argument) {
  return argument == "version" || argument == "--version" || argument == "-V";
}

[[nodiscard]] int run_target(const std::span<const std::string_view> arguments,
                             std::ostream& error) {
  if (arguments.size() < 3U || arguments[1] != "--") {
    error << "usage: retrace run -- COMMAND [ARGS...]\n";
    return static_cast<int>(ExitCode::usage_error);
  }

  const auto target_arguments = arguments.subspan(2);
  const auto result = process::execute(target_arguments);

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
    return run_target(arguments, error);
  }

  error << "error: unknown command '" << arguments.front() << "'\n"
        << "try 'retrace help' for usage\n";
  return static_cast<int>(ExitCode::usage_error);
}

}  // namespace retrace::cli
