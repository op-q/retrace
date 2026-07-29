#pragma once

// Public process-supervision API. The implementation owns Linux descriptors and
// processes; callers receive borrowed event views and one final owned result.

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <system_error>

namespace retrace::process {

enum class ProcessState : std::uint8_t {
  // `exited` and `signaled` describe the target. The failure states distinguish
  // target launch errors from failures inside RETRACE's supervisor.
  exited,
  signaled,
  launch_failed,
  supervisor_failed,
};

enum class ProcessEventType : std::uint8_t {
  started,
  exec_succeeded,
  runtime_handshake,
  standard_output,
  standard_error,
  signal_forwarded,
  exited,
  signaled,
  launch_failed,
};

struct ProcessEvent {
  // `value` is interpreted by `type` (exit code, signal, or errno). `bytes` is
  // populated only for stream chunks and is borrowed during the callback.
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
  // Only fields relevant to `state` are meaningful. `process_id` remains zero
  // when validation fails before fork.
  ProcessState state = ProcessState::supervisor_failed;
  int exit_code = 0;
  int signal_number = 0;
  int process_id = 0;
  std::error_code error;
};

struct ExecuteOptions {
  // Empty means inherit the caller's working directory. The byte view must not
  // contain NUL because chdir(2) consumes a NUL-terminated path.
  std::string_view working_directory;
};

// An empty list or an argument containing an embedded NUL is rejected before
// fork because execvpe(3) accepts only NUL-terminated argument strings.
[[nodiscard]] ProcessResult execute(std::span<const std::string_view> arguments,
                                    const ProcessEventHandler& event_handler = {},
                                    const ExecuteOptions& options = {});

}  // namespace retrace::process
