// End-to-end tests for the Linux supervisor: fork/exec status, process groups,
// signals, working directories, stream draining, runtime IPC, and error priority.

#include "retrace/process.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "retrace/runtime_protocol.h"

#ifndef RETRACE_STREAM_FIXTURE_PATH
#error "RETRACE_STREAM_FIXTURE_PATH must name the stream fixture executable"
#endif

#ifndef RETRACE_SIGNAL_FIXTURE_PATH
#error "RETRACE_SIGNAL_FIXTURE_PATH must name the signal fixture executable"
#endif

#ifndef RETRACE_RUNTIME_CHANNEL_FIXTURE_PATH
#error "RETRACE_RUNTIME_CHANNEL_FIXTURE_PATH must name the runtime channel fixture"
#endif

#ifndef RETRACE_RUNTIME_LIBRARY_PATH
#error "RETRACE_RUNTIME_LIBRARY_PATH must name the production runtime library"
#endif

#ifndef RETRACE_PRELOAD_FIXTURE_PATH
#error "RETRACE_PRELOAD_FIXTURE_PATH must name the harmless preload fixture"
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

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    path_ = "/tmp/retrace-process-test-XXXXXX";
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

class ScopedEnvironment final {
 public:
  ScopedEnvironment(const char* const name, const std::string_view value)
      : name_{name} {
    if (const char* const previous = ::getenv(name_.c_str()); previous != nullptr) {
      previous_value_ = previous;
    }
    configured_ = ::setenv(name_.c_str(), std::string{value}.c_str(), 1) == 0;
  }

  ~ScopedEnvironment() {
    if (!configured_) {
      return;
    }
    if (previous_value_) {
      [[maybe_unused]] const auto result =
          ::setenv(name_.c_str(), previous_value_->c_str(), 1);
    } else {
      [[maybe_unused]] const auto result = ::unsetenv(name_.c_str());
    }
  }

  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

  [[nodiscard]] bool configured() const noexcept { return configured_; }

 private:
  std::string name_;
  std::optional<std::string> previous_value_;
  bool configured_ = false;
};

struct ObservedEvent {
  retrace::process::ProcessEventType type;
  int process_id;
  int value;
};

struct CapturedOutput {
  std::string standard_output;
  std::string standard_error;

  std::error_code append(const retrace::process::ProcessEvent& event) {
    if (event.type != retrace::process::ProcessEventType::standard_output &&
        event.type != retrace::process::ProcessEventType::standard_error) {
      return {};
    }
    auto& destination =
        event.type == retrace::process::ProcessEventType::standard_output
            ? standard_output
            : standard_error;
    destination.append(event.bytes);
    return {};
  }
};

void test_empty_command_is_rejected(TestContext& test) {
  const auto result = retrace::process::execute(std::span<const std::string_view>{});

  test.expect(result.state == retrace::process::ProcessState::supervisor_failed,
              "an empty command is a supervisor error");
  test.expect(result.error == std::make_error_code(std::errc::invalid_argument),
              "an empty command reports invalid_argument");
}

void test_embedded_nul_argument_is_rejected(TestContext& test) {
  const std::string command_with_nul{"/bin/true\0ignored", 17U};
  const std::array arguments{std::string_view{command_with_nul}};

  const auto result = retrace::process::execute(arguments);

  test.expect(result.state == retrace::process::ProcessState::supervisor_failed,
              "an embedded NUL is rejected before launching a target");
  test.expect(result.error == std::make_error_code(std::errc::invalid_argument),
              "an embedded NUL reports invalid_argument");
  test.expect(result.process_id == 0,
              "invalid target bytes do not create a child process");
}

void test_runtime_handshake_is_received(TestContext& test) {
  ScopedEnvironment caller_environment{RETRACE_RUNTIME_EVENT_FD_ENV,
                                       "caller-value-must-be-replaced"};
  test.expect(caller_environment.configured(),
              "the runtime environment replacement test is configured");
  if (!caller_environment.configured()) {
    return;
  }

  constexpr std::array arguments{std::string_view{RETRACE_RUNTIME_CHANNEL_FIXTURE_PATH},
                                 std::string_view{"load-runtime"}};
  std::vector<ObservedEvent> events;
  const auto result = retrace::process::execute(
      arguments, [&events](const retrace::process::ProcessEvent& event) {
        events.push_back(
            {.type = event.type, .process_id = event.process_id, .value = event.value});
        return std::error_code{};
      });

  const auto handshake_count =
      std::count_if(events.begin(), events.end(), [](const auto& event) {
        return event.type == retrace::process::ProcessEventType::runtime_handshake;
      });
  const auto exec_position =
      std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.type == retrace::process::ProcessEventType::exec_succeeded;
      });
  const auto handshake_position =
      std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.type == retrace::process::ProcessEventType::runtime_handshake;
      });
  const auto exit_position =
      std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.type == retrace::process::ProcessEventType::exited;
      });

  test.expect(
      result.state == retrace::process::ProcessState::exited && result.exit_code == 0,
      "a target that loads the runtime exits normally");
  test.expect(handshake_count == 1,
              "the supervisor receives exactly one runtime handshake");
  test.expect(exec_position < handshake_position && handshake_position < exit_position,
              "the runtime handshake follows exec and precedes target exit");
}

void test_runtime_is_automatically_loaded(TestContext& test) {
  constexpr std::array arguments{std::string_view{RETRACE_RUNTIME_CHANNEL_FIXTURE_PATH},
                                 std::string_view{"no-handshake"}};
  std::vector<ObservedEvent> events;
  const auto result = retrace::process::execute(
      arguments,
      [&events](const retrace::process::ProcessEvent& event) {
        events.push_back(
            {.type = event.type, .process_id = event.process_id, .value = event.value});
        return std::error_code{};
      },
      {.runtime_library = RETRACE_RUNTIME_LIBRARY_PATH});

  const auto handshake_count =
      std::count_if(events.begin(), events.end(), [](const auto& event) {
        return event.type == retrace::process::ProcessEventType::runtime_handshake;
      });
  test.expect(
      result.state == retrace::process::ProcessState::exited && result.exit_code == 0,
      "an injected runtime target exits normally");
  test.expect(handshake_count == 1,
              "LD_PRELOAD automatically produces one validated runtime handshake");
}

void test_exec_descendant_does_not_duplicate_the_handshake(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/bin/sh"}, std::string_view{"-c"},
                                 std::string_view{"/bin/true"}};
  std::size_t handshake_count = 0U;
  const auto result = retrace::process::execute(
      arguments,
      [&handshake_count](const retrace::process::ProcessEvent& event) {
        if (event.type == retrace::process::ProcessEventType::runtime_handshake) {
          ++handshake_count;
        }
        return std::error_code{};
      },
      {.runtime_library = RETRACE_RUNTIME_LIBRARY_PATH});

  test.expect(
      result.state == retrace::process::ProcessState::exited && result.exit_code == 0,
      "an exec'd descendant does not turn runtime loading into a protocol error");
  test.expect(handshake_count == 1,
              "close-on-exec confines the session handshake to the direct image");
}

void test_runtime_preload_preserves_the_caller_value(TestContext& test) {
  ScopedEnvironment caller_preload{"LD_PRELOAD", RETRACE_PRELOAD_FIXTURE_PATH};
  test.expect(caller_preload.configured(),
              "the caller preload preservation test is configured");
  if (!caller_preload.configured()) {
    return;
  }

  const std::string expected_preload =
      std::string{RETRACE_RUNTIME_LIBRARY_PATH} + ':' + RETRACE_PRELOAD_FIXTURE_PATH;
  const std::array arguments{std::string_view{RETRACE_RUNTIME_CHANNEL_FIXTURE_PATH},
                             std::string_view{"expect-preload"},
                             std::string_view{expected_preload}};
  const auto result = retrace::process::execute(
      arguments, {}, {.runtime_library = RETRACE_RUNTIME_LIBRARY_PATH});

  test.expect(
      result.state == retrace::process::ProcessState::exited && result.exit_code == 0,
      "runtime injection prepends RETRACE and preserves the caller's preload");
}

void test_missing_required_runtime_is_reported(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/bin/true"}};
  std::vector<ObservedEvent> events;
  const auto result = retrace::process::execute(
      arguments,
      [&events](const retrace::process::ProcessEvent& event) {
        events.push_back(
            {.type = event.type, .process_id = event.process_id, .value = event.value});
        return std::error_code{};
      },
      {.runtime_library = RETRACE_PRELOAD_FIXTURE_PATH});

  const auto exit_event =
      std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.type == retrace::process::ProcessEventType::exited;
      });
  test.expect(result.state == retrace::process::ProcessState::runtime_unavailable,
              "a target without the required handshake is explicitly unsupported");
  test.expect(result.exit_code == 0 &&
                  result.error == std::make_error_code(std::errc::not_supported),
              "runtime unavailability preserves the observed target result");
  test.expect(exit_event != events.end(),
              "runtime unavailability does not discard target lifecycle evidence");
}

void test_runtime_channel_can_be_disabled(TestContext& test) {
  ScopedEnvironment caller_environment{RETRACE_RUNTIME_EVENT_FD_ENV,
                                       "caller-value-must-be-removed"};
  test.expect(caller_environment.configured(),
              "the disabled runtime channel test is configured");
  if (!caller_environment.configured()) {
    return;
  }

  constexpr std::array arguments{std::string_view{RETRACE_RUNTIME_CHANNEL_FIXTURE_PATH},
                                 std::string_view{"no-channel"}};
  const auto result =
      retrace::process::execute(arguments, {}, {.runtime_channel_enabled = false});

  test.expect(
      result.state == retrace::process::ProcessState::exited && result.exit_code == 0,
      "disabling the runtime removes the owned channel from the target");
}

void test_invalid_runtime_library_is_rejected(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/bin/true"}};
  const auto result = retrace::process::execute(
      arguments, {}, {.runtime_library = "/tmp/not:a-library.so"});

  test.expect(result.state == retrace::process::ProcessState::supervisor_failed,
              "an unrepresentable preload path is rejected");
  test.expect(result.process_id == 0 &&
                  result.error == std::make_error_code(std::errc::invalid_argument),
              "invalid preload bytes are rejected before fork");
}

void test_absent_runtime_handshake_is_nonfatal(TestContext& test) {
  constexpr std::array arguments{std::string_view{RETRACE_RUNTIME_CHANNEL_FIXTURE_PATH},
                                 std::string_view{"no-handshake"}};
  std::size_t handshake_count = 0U;
  const auto result = retrace::process::execute(
      arguments, [&handshake_count](const retrace::process::ProcessEvent& event) {
        if (event.type == retrace::process::ProcessEventType::runtime_handshake) {
          ++handshake_count;
        }
        return std::error_code{};
      });

  test.expect(
      result.state == retrace::process::ProcessState::exited && result.exit_code == 0,
      "a target may ignore the runtime channel");
  test.expect(handshake_count == 0U,
              "an ignored runtime channel creates no false handshake");
}

void test_invalid_runtime_messages_are_rejected(TestContext& test) {
  struct InvalidCase {
    std::string_view mode;
    std::error_code expected_error;
  };
  const std::array cases{
      InvalidCase{"bad-magic", std::make_error_code(std::errc::protocol_error)},
      InvalidCase{"unsupported-major", std::make_error_code(std::errc::protocol_error)},
      InvalidCase{"unsupported-minor", std::make_error_code(std::errc::protocol_error)},
      InvalidCase{"nonzero-flags", std::make_error_code(std::errc::protocol_error)},
      InvalidCase{"unknown-type", std::make_error_code(std::errc::protocol_error)},
      InvalidCase{"payload-mismatch", std::make_error_code(std::errc::protocol_error)},
      InvalidCase{"short-frame", std::make_error_code(std::errc::protocol_error)},
      InvalidCase{"oversized", std::make_error_code(std::errc::message_size)},
      InvalidCase{"duplicate-handshake",
                  std::make_error_code(std::errc::protocol_error)},
  };

  for (const auto& invalid : cases) {
    const std::array arguments{std::string_view{RETRACE_RUNTIME_CHANNEL_FIXTURE_PATH},
                               invalid.mode};
    bool exit_observed = false;
    const auto result = retrace::process::execute(
        arguments, [&exit_observed](const retrace::process::ProcessEvent& event) {
          if (event.type == retrace::process::ProcessEventType::exited) {
            exit_observed = true;
          }
          return std::error_code{};
        });

    test.expect(result.state == retrace::process::ProcessState::supervisor_failed,
                "an invalid runtime message fails supervision");
    test.expect(result.error == invalid.expected_error,
                "an invalid runtime message reports its protocol class");
    test.expect(exit_observed,
                "the target exit is still observed after a runtime protocol error");
  }
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

void test_working_directory_is_selected(TestContext& test) {
  TemporaryDirectory directory;
  test.expect(!directory.value().empty(),
              "a temporary target working directory is available");
  if (directory.value().empty()) {
    return;
  }

  constexpr std::array arguments{std::string_view{"/bin/pwd"}};
  CapturedOutput captured;
  const auto result = retrace::process::execute(
      arguments,
      [&captured](const retrace::process::ProcessEvent& event) {
        return captured.append(event);
      },
      {.working_directory = directory.value()});

  test.expect(
      result.state == retrace::process::ProcessState::exited && result.exit_code == 0,
      "a target launches in the selected working directory");
  test.expect(captured.standard_output == directory.value() + "\n",
              "the target observes the selected working directory");
}

void test_invalid_working_directory_is_a_launch_failure(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/bin/true"}};
  const auto result = retrace::process::execute(
      arguments, {},
      {.working_directory = "/definitely/not/a/retrace-working-directory"});

  test.expect(result.state == retrace::process::ProcessState::launch_failed,
              "a failed child chdir is a target launch failure");
  test.expect(
      result.error == std::make_error_code(std::errc::no_such_file_or_directory),
      "a failed child chdir preserves ENOENT");
}

void test_embedded_nul_working_directory_is_rejected(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/bin/true"}};
  const std::string path_with_nul{"/tmp\0ignored", 12U};
  const auto result =
      retrace::process::execute(arguments, {}, {.working_directory = path_with_nul});

  test.expect(result.state == retrace::process::ProcessState::supervisor_failed,
              "an embedded NUL working directory is rejected by the supervisor");
  test.expect(result.error == std::make_error_code(std::errc::invalid_argument) &&
                  result.process_id == 0,
              "an invalid working-directory byte view is rejected before fork");
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

void test_signal_is_forwarded_to_target_group(TestContext& test,
                                              const int signal_number,
                                              const std::string_view fixture_option) {
  std::array<int, 2> report_pipe{-1, -1};
  test.expect(::pipe(report_pipe.data()) == 0,
              "a report pipe is available for the signal-forwarding test");
  if (report_pipe[0] < 0 || report_pipe[1] < 0) {
    return;
  }

  const pid_t test_child = ::fork();
  test.expect(test_child >= 0, "the signal-forwarding supervisor can be isolated");
  if (test_child < 0) {
    ::close(report_pipe[0]);
    ::close(report_pipe[1]);
    return;
  }

  if (test_child == 0) {
    ::close(report_pipe[0]);
    const std::array arguments{std::string_view{RETRACE_SIGNAL_FIXTURE_PATH},
                               fixture_option};
    CapturedOutput captured;
    int forwarded_count = 0;
    int forwarded_value = 0;
    bool signal_sent = false;

    const auto result = retrace::process::execute(
        arguments, [&](const retrace::process::ProcessEvent& event) {
          const auto append_error = captured.append(event);
          if (event.type == retrace::process::ProcessEventType::signal_forwarded) {
            ++forwarded_count;
            forwarded_value = event.value;
          }
          if (!signal_sent &&
              event.type == retrace::process::ProcessEventType::standard_output &&
              event.bytes.find("ready\n") != std::string_view::npos) {
            signal_sent = ::kill(::getpid(), signal_number) == 0;
          }
          return append_error;
        });

    sigset_t current_mask{};
    const bool mask_read = ::sigprocmask(SIG_SETMASK, nullptr, &current_mask) == 0;
    const std::array report{
        static_cast<int>(result.state),
        result.exit_code,
        forwarded_count,
        forwarded_value,
        signal_sent ? 1 : 0,
        captured.standard_output == "ready\nforwarded\n" ? 1 : 0,
        mask_read && ::sigismember(&current_mask, signal_number) == 0 ? 1 : 0};
    const auto ignored = ::write(report_pipe[1], report.data(), sizeof(report));
    static_cast<void>(ignored);
    ::_exit(0);
  }

  ::close(report_pipe[1]);
  std::array<int, 7> report{};
  const auto bytes_read = ::read(report_pipe[0], report.data(), sizeof(report));
  ::close(report_pipe[0]);
  int wait_status = 0;
  const auto waited = ::waitpid(test_child, &wait_status, 0);

  test.expect(waited == test_child && WIFEXITED(wait_status),
              "the isolated signal-forwarding supervisor exits normally");
  test.expect(bytes_read == static_cast<ssize_t>(sizeof(report)),
              "the isolated supervisor reports its forwarding result");
  if (bytes_read != static_cast<ssize_t>(sizeof(report))) {
    return;
  }
  test.expect(report[0] == static_cast<int>(retrace::process::ProcessState::exited) &&
                  report[1] == 0,
              "the signal-aware target group handles the forwarded signal");
  test.expect(report[2] == 1 && report[3] == signal_number,
              "signal forwarding emits exactly one event with the signal number");
  test.expect(report[4] == 1, "the supervisor process receives the test signal");
  test.expect(report[5] == 1,
              "the target leader and descendant both observe the forwarded signal");
  test.expect(report[6] == 1,
              "the caller's original signal mask is restored after supervision");
}

void test_missing_executable(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/definitely/not/a/retrace-command"}};
  std::vector<ObservedEvent> events;

  const auto result = retrace::process::execute(
      arguments, [&events](const retrace::process::ProcessEvent& event) {
        events.push_back(
            {.type = event.type, .process_id = event.process_id, .value = event.value});
        return std::error_code{};
      });

  test.expect(result.state == retrace::process::ProcessState::launch_failed,
              "a missing executable is a launch failure");
  test.expect(
      result.error == std::make_error_code(std::errc::no_such_file_or_directory),
      "a missing executable preserves ENOENT");
  test.expect(result.process_id > 0,
              "a launch failure still identifies the forked process");
  test.expect(events.size() == 2U,
              "a launch failure emits start and launch-failure events");
  if (events.size() == 2U) {
    test.expect(events[0].type == retrace::process::ProcessEventType::started,
                "a failed launch begins with process start");
    test.expect(events[1].type == retrace::process::ProcessEventType::launch_failed,
                "a failed exec emits a launch-failure event");
    test.expect(events[1].value == ENOENT, "the launch-failure event preserves errno");
  }
}

void test_handler_failure_overrides_launch_failure(TestContext& test) {
  constexpr std::array arguments{
      std::string_view{"/definitely/not/a/retrace-command-with-handler-failure"}};
  std::size_t handler_calls = 0U;

  const auto result = retrace::process::execute(
      arguments, [&handler_calls](const retrace::process::ProcessEvent&) {
        ++handler_calls;
        return std::make_error_code(std::errc::no_space_on_device);
      });

  test.expect(result.state == retrace::process::ProcessState::supervisor_failed,
              "a failed lifecycle consumer is a supervisor failure");
  test.expect(result.error == std::make_error_code(std::errc::no_space_on_device),
              "the lifecycle consumer error is not hidden by exec failure");
  test.expect(handler_calls == 1U,
              "event delivery stops after the first lifecycle consumer error");
}

void test_process_lifecycle_events(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/bin/true"}};
  std::vector<ObservedEvent> events;

  const auto result = retrace::process::execute(
      arguments, [&events](const retrace::process::ProcessEvent& event) {
        events.push_back(
            {.type = event.type, .process_id = event.process_id, .value = event.value});
        return std::error_code{};
      });

  test.expect(result.state == retrace::process::ProcessState::exited,
              "the lifecycle target exits normally");
  test.expect(result.process_id > 0, "the process result includes the target pid");
  test.expect(events.size() == 3U, "a clean target emits start, exec, and exit events");
  if (events.size() == 3U) {
    test.expect(events[0].type == retrace::process::ProcessEventType::started,
                "the first lifecycle event records process start");
    test.expect(events[1].type == retrace::process::ProcessEventType::exec_succeeded,
                "the second lifecycle event confirms exec");
    test.expect(events[2].type == retrace::process::ProcessEventType::exited,
                "the final lifecycle event records ordinary exit");
    test.expect(events[0].process_id == result.process_id &&
                    events[1].process_id == result.process_id &&
                    events[2].process_id == result.process_id,
                "all lifecycle events identify the supervised process");
    test.expect(events[2].value == 0,
                "the exit lifecycle event preserves the target status");
  }
}

void test_output_streams_are_captured_separately(TestContext& test) {
  constexpr std::array arguments{std::string_view{RETRACE_STREAM_FIXTURE_PATH}};
  CapturedOutput captured;

  const auto result = retrace::process::execute(
      arguments, [&captured](const retrace::process::ProcessEvent& event) {
        return captured.append(event);
      });

  test.expect(result.state == retrace::process::ProcessState::exited,
              "the stream fixture exits normally while captured");
  test.expect(result.exit_code == 0, "capturing output preserves a clean exit");
  test.expect(captured.standard_output == "fixture: stdout\n",
              "stdout bytes are delivered to the stdout channel");
  test.expect(captured.standard_error == "fixture: stderr\n",
              "stderr bytes are delivered to the stderr channel");
}

void test_large_output_does_not_fill_a_pipe(TestContext& test) {
  constexpr std::size_t expected_stream_size = std::size_t{4096U} * 32U;
  constexpr std::array arguments{std::string_view{RETRACE_STREAM_FIXTURE_PATH},
                                 std::string_view{"--large"}};
  CapturedOutput captured;

  const auto result = retrace::process::execute(
      arguments, [&captured](const retrace::process::ProcessEvent& event) {
        return captured.append(event);
      });

  test.expect(result.state == retrace::process::ProcessState::exited,
              "a target can write more than one pipe's capacity");
  test.expect(result.exit_code == 0, "large stream capture preserves a clean exit");
  test.expect(captured.standard_output.size() == expected_stream_size,
              "all large stdout bytes are captured");
  test.expect(captured.standard_error.size() == expected_stream_size,
              "all large stderr bytes are captured");
  test.expect(captured.standard_output.find_first_not_of('o') == std::string::npos,
              "large stdout content is not corrupted");
  test.expect(captured.standard_error.find_first_not_of('e') == std::string::npos,
              "large stderr content is not corrupted");
}

void test_descendant_does_not_hold_capture_open(TestContext& test) {
  constexpr std::array arguments{std::string_view{"/bin/sh"}, std::string_view{"-c"},
                                 std::string_view{"/bin/sleep 1 & descendant=$!; "
                                                  "printf '%d\\n' \"$descendant\"; "
                                                  "printf 'direct stderr\\n' >&2"}};
  CapturedOutput captured;

  const auto result = retrace::process::execute(
      arguments, [&captured](const retrace::process::ProcessEvent& event) {
        return captured.append(event);
      });

  test.expect(result.state == retrace::process::ProcessState::exited,
              "a direct target exits while a descendant retains its streams");
  test.expect(result.exit_code == 0,
              "a descendant-held stream does not change the target exit code");
  test.expect(captured.standard_error == "direct stderr\n",
              "stderr buffered before direct-target exit is preserved");

  int descendant = 0;
  const auto* output_begin = captured.standard_output.data();
  const auto* output_end = output_begin + captured.standard_output.size();
  const auto parsed = std::from_chars(output_begin, output_end, descendant);
  const bool captured_pid = parsed.ec == std::errc{} && parsed.ptr != output_end &&
                            parsed.ptr + 1 == output_end && *parsed.ptr == '\n' &&
                            descendant > 1;
  test.expect(captured_pid,
              "stdout buffered before direct-target exit preserves the descendant pid");

  if (captured_pid) {
    test.expect(::kill(descendant, 0) == 0,
                "capture returns while the descriptor-holding descendant is alive");
    test.expect(::kill(descendant, SIGTERM) == 0,
                "the descriptor-holding test descendant is stopped immediately");
  }
}

void test_output_handler_failure_is_reported_after_draining(TestContext& test) {
  constexpr std::array arguments{std::string_view{RETRACE_STREAM_FIXTURE_PATH},
                                 std::string_view{"--large"}};
  std::size_t handler_calls = 0U;

  const auto result = retrace::process::execute(
      arguments, [&handler_calls](const retrace::process::ProcessEvent& event) {
        if (event.type != retrace::process::ProcessEventType::standard_output &&
            event.type != retrace::process::ProcessEventType::standard_error) {
          return std::error_code{};
        }
        ++handler_calls;
        return std::make_error_code(std::errc::no_space_on_device);
      });

  test.expect(result.state == retrace::process::ProcessState::supervisor_failed,
              "a failed output consumer is a supervisor failure");
  test.expect(result.error == std::make_error_code(std::errc::no_space_on_device),
              "the output consumer's error is preserved");
  test.expect(handler_calls == 1U,
              "the collector drains without calling a failed consumer again");
}

void test_launch_failure_with_closed_standard_streams(TestContext& test) {
  std::array<int, 2> report_pipe{-1, -1};
  test.expect(::pipe(report_pipe.data()) == 0,
              "a report pipe is available for the closed-stream test");
  if (report_pipe[0] < 0 || report_pipe[1] < 0) {
    return;
  }

  const pid_t test_child = ::fork();
  test.expect(test_child >= 0, "the closed-stream test process can be created");
  if (test_child < 0) {
    ::close(report_pipe[0]);
    ::close(report_pipe[1]);
    return;
  }

  if (test_child == 0) {
    ::close(report_pipe[0]);
    ::close(STDOUT_FILENO);
    ::close(STDERR_FILENO);
    constexpr std::array arguments{
        std::string_view{"/definitely/not/a/retrace-command"}};
    const auto result = retrace::process::execute(arguments);
    const std::array report{static_cast<int>(result.state), result.error.value()};
    const auto ignored = ::write(report_pipe[1], report.data(), sizeof(report));
    static_cast<void>(ignored);
    ::_exit(0);
  }

  ::close(report_pipe[1]);
  std::array<int, 2> report{};
  const auto bytes_read = ::read(report_pipe[0], report.data(), sizeof(report));
  ::close(report_pipe[0]);
  int wait_status = 0;
  const auto waited = ::waitpid(test_child, &wait_status, 0);

  test.expect(bytes_read == static_cast<ssize_t>(sizeof(report)),
              "the nested supervisor reports its result");
  test.expect(waited == test_child && WIFEXITED(wait_status),
              "the closed-stream test process exits normally");
  test.expect(
      report[0] == static_cast<int>(retrace::process::ProcessState::launch_failed),
      "closed stdout and stderr do not hide an exec failure");
  test.expect(report[1] == ENOENT,
              "closed stdout and stderr preserve the launch errno");
}

}  // namespace

int main() {
  TestContext test;
  test_empty_command_is_rejected(test);
  test_embedded_nul_argument_is_rejected(test);
  test_runtime_handshake_is_received(test);
  test_runtime_is_automatically_loaded(test);
  test_exec_descendant_does_not_duplicate_the_handshake(test);
  test_runtime_preload_preserves_the_caller_value(test);
  test_missing_required_runtime_is_reported(test);
  test_runtime_channel_can_be_disabled(test);
  test_invalid_runtime_library_is_rejected(test);
  test_absent_runtime_handshake_is_nonfatal(test);
  test_invalid_runtime_messages_are_rejected(test);
  test_clean_exit(test);
  test_nonzero_exit(test);
  test_exit_127_is_not_a_launch_failure(test);
  test_arguments_are_preserved(test);
  test_working_directory_is_selected(test);
  test_invalid_working_directory_is_a_launch_failure(test);
  test_embedded_nul_working_directory_is_rejected(test);
  test_signal_exit(test);
  test_signal_is_forwarded_to_target_group(test, SIGINT, "--expect-int");
  test_signal_is_forwarded_to_target_group(test, SIGTERM, "--expect-term");
  test_missing_executable(test);
  test_handler_failure_overrides_launch_failure(test);
  test_process_lifecycle_events(test);
  test_output_streams_are_captured_separately(test);
  test_large_output_does_not_fill_a_pipe(test);
  test_descendant_does_not_hold_capture_open(test);
  test_output_handler_failure_is_reported_after_draining(test);
  test_launch_failure_with_closed_standard_streams(test);
  return test.result();
}
