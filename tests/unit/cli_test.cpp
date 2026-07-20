#include "retrace/cli.hpp"

#include <array>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include "retrace/version.hpp"

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

void test_run_requires_a_target_after_separator(TestContext& test) {
  constexpr std::array arguments{std::string_view{"run"}, std::string_view{"--"}};
  std::ostringstream output;
  std::ostringstream error;

  const auto result = retrace::cli::run(arguments, output, error);

  test.expect(result == static_cast<int>(retrace::cli::ExitCode::usage_error),
              "run without a target returns a usage error");
  test.expect(output.str().empty(), "invalid run syntax has no normal output");
  test.expect(error.str() == "usage: retrace run -- COMMAND [ARGS...]\n",
              "invalid run syntax shows the exact command shape");
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

}  // namespace

int main() {
  TestContext test;
  test_empty_arguments_show_help(test);
  test_version(test);
  test_unknown_command(test);
  test_version_rejects_extra_arguments(test);
  test_run_requires_a_target_after_separator(test);
  test_run_preserves_target_exit_code(test);
  test_run_reports_a_missing_executable(test);
  return test.result();
}
