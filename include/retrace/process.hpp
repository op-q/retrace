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
  // target launch errors, unavailable requested instrumentation, and failures
  // inside RETRACE's supervisor.
  exited,
  signaled,
  launch_failed,
  runtime_unavailable,
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
  // Explicit aggregate defaults prevent GCC's missing-field warning when a
  // caller designates only one later option.
  std::string_view working_directory{};  // NOLINT(readability-redundant-member-init)

  // A non-empty absolute path is prepended to LD_PRELOAD and makes one validated
  // runtime handshake mandatory. Loader token separators are rejected because
  // LD_PRELOAD has no escaping for paths containing them.
  std::string_view runtime_library{};  // NOLINT(readability-redundant-member-init)

  // Channel-only mode is the default for API compatibility and focused runtime
  // tests. Setting this false also requires an empty runtime_library.
  bool runtime_channel_enabled = true;
};

// An empty list or an argument containing an embedded NUL is rejected before
// fork because execvpe(3) accepts only NUL-terminated argument strings.
[[nodiscard]] ProcessResult execute(std::span<const std::string_view> arguments,
                                    const ProcessEventHandler& event_handler = {},
                                    const ExecuteOptions& options = {});

}  // namespace retrace::process
