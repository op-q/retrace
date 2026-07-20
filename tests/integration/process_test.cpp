#include "retrace/process.hpp"

#include <array>
#include <csignal>
#include <iostream>
#include <span>
#include <string_view>
#include <system_error>

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

void test_empty_command_is_rejected(TestContext& test) {
  const auto result = retrace::process::execute(std::span<const std::string_view>{});

  test.expect(result.state == retrace::process::ProcessState::supervisor_failed,
              "an empty command is a supervisor error");
  test.expect(result.error == std::make_error_code(std::errc::invalid_argument),
              "an empty command reports invalid_argument");
}

void test_clean_exit(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/bin/true"}};

  const auto result = retrace::process::execute(arguments);

  test.expect(result.state == retrace::process::ProcessState::exited,
              "a clean target reports an ordinary exit");
  test.expect(result.exit_code == 0, "a clean target preserves exit code zero");
}

void test_nonzero_exit(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/bin/sh"}, std::string_view{"-c"},
                                 std::string_view{"exit 7"}};

  const auto result = retrace::process::execute(arguments);

  test.expect(result.state == retrace::process::ProcessState::exited,
              "a failing target still reports an ordinary exit");
  test.expect(result.exit_code == 7, "the target's non-zero exit code is preserved");
}

void test_exit_127_is_not_a_launch_failure(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/bin/sh"}, std::string_view{"-c"},
                                 std::string_view{"exit 127"}};

  const auto result = retrace::process::execute(arguments);

  test.expect(result.state == retrace::process::ProcessState::exited,
              "target exit 127 remains an ordinary exit");
  test.expect(result.exit_code == 127, "target exit code 127 is preserved");
}

void test_arguments_are_preserved(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/bin/sh"}, std::string_view{"-c"},
                                 std::string_view{"test \"$1\" = \"hello world\""},
                                 std::string_view{"retrace-test"},
                                 std::string_view{"hello world"}};

  const auto result = retrace::process::execute(arguments);

  test.expect(result.state == retrace::process::ProcessState::exited,
              "an argument-checking target exits normally");
  test.expect(result.exit_code == 0, "spaces inside a target argument are preserved");
}

void test_signal_exit(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/bin/sh"}, std::string_view{"-c"},
                                 std::string_view{"kill -TERM $$"}};

  const auto result = retrace::process::execute(arguments);

  test.expect(result.state == retrace::process::ProcessState::signaled,
              "a signaled target is distinct from an ordinary exit");
  test.expect(result.signal_number == SIGTERM,
              "the terminating signal number is preserved");
}

void test_missing_executable(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/definitely/not/a/retrace-command"}};

  const auto result = retrace::process::execute(arguments);

  test.expect(result.state == retrace::process::ProcessState::launch_failed,
              "a missing executable is a launch failure");
  test.expect(
      result.error == std::make_error_code(std::errc::no_such_file_or_directory),
      "a missing executable preserves ENOENT");
}

}  // namespace

int main() {
  TestContext test;
  test_empty_command_is_rejected(test);
  test_clean_exit(test);
  test_nonzero_exit(test);
  test_exit_127_is_not_a_launch_failure(test);
  test_arguments_are_preserved(test);
  test_signal_exit(test);
  test_missing_executable(test);
  return test.result();
}
