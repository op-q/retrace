#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <system_error>

namespace retrace::process {

enum class ProcessState : std::uint8_t {
  exited,
  signaled,
  launch_failed,
  supervisor_failed,
};

enum class ProcessEventType : std::uint8_t {
  started,
  exec_succeeded,
  standard_output,
  standard_error,
  exited,
  signaled,
  launch_failed,
};

struct ProcessEvent {
  ProcessEventType type = ProcessEventType::started;
  int process_id = 0;
  int value = 0;
  std::string_view bytes;
};

// Event byte views remain valid only for the duration of the call. Returning an
// error stops further delivery while the supervisor continues collecting until
// the direct target exits, drains bytes already queued, and reaps it.
using ProcessEventHandler = std::function<std::error_code(const ProcessEvent&)>;

struct ProcessResult {
  ProcessState state = ProcessState::supervisor_failed;
  int exit_code = 0;
  int signal_number = 0;
  int process_id = 0;
  std::error_code error;
};

// An empty list or an argument containing an embedded NUL is rejected before
// fork because execvp(3) accepts only NUL-terminated argument strings.
[[nodiscard]] ProcessResult execute(std::span<const std::string_view> arguments,
                                    const ProcessEventHandler& event_handler = {});

}  // namespace retrace::process
