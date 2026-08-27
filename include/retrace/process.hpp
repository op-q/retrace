#pragma once

// Public process-supervision API. The implementation owns Linux descriptors and
// processes; callers receive borrowed event views and one final owned result.

#include <cstdint>
#include <functional>
#include <span>
#include <string>
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
  runtime_operation,
  standard_output,
  standard_error,
  signal_forwarded,
  exited,
  signaled,
  launch_failed,
};

// One libc operation the injected runtime observed inside the target. Only the
// fields meaningful for `kind` carry information; the rest stay zero, so a
// consumer never has to guess which field a given operation populated.
//
// `sequence` is assigned by the runtime and counts operations it attempted to
// report. A gap therefore means the bounded channel dropped frames, which the
// supervisor reports rather than presenting an incomplete record as complete.
enum class RuntimeOperationKind : std::uint16_t {  // NOLINT(performance-enum-size)
  file_open = 1U,
  file_openat = 2U,
  file_close = 3U,
};

struct RuntimeOperation {
  std::uint64_t sequence = 0U;
  std::uint64_t monotonic_nanoseconds = 0U;
  std::uint64_t duration_nanoseconds = 0U;
  std::uint32_t thread_id = 0U;
  RuntimeOperationKind kind = RuntimeOperationKind::file_open;
  // Set when the target used a path longer than the recorded bound. The stored
  // path is then a prefix, not the argument the target actually passed.
  bool path_truncated = false;
  std::int64_t result = 0;
  // Meaningful only when `result` is negative.
  std::uint32_t error_number = 0U;
  std::int32_t descriptor = 0;
  std::int32_t directory = 0;
  std::uint32_t open_flags = 0U;
  std::uint32_t mode = 0U;
  std::string path;
};

struct ProcessEvent {
  // `value` is interpreted by `type` (exit code, signal, or errno). `bytes` is
  // populated only for stream chunks and is borrowed during the callback.
  // `operation` is non-null only for `runtime_operation` and is likewise
  // borrowed: the supervisor reuses one instance across events.
  ProcessEventType type = ProcessEventType::started;
  int process_id = 0;
  int value = 0;
  std::string_view bytes;
  const RuntimeOperation* operation = nullptr;
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
  // Operations the runtime reported that never reached the supervisor, counted
  // from gaps in its sequence numbering. A nonzero value means the bounded
  // channel dropped events and the observed record is therefore incomplete.
  std::uint64_t dropped_runtime_operations = 0U;
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
