#pragma once

#include <cstdint>
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

struct ProcessResult {
  ProcessState state = ProcessState::supervisor_failed;
  int exit_code = 0;
  int signal_number = 0;
  std::error_code error;
};

[[nodiscard]] ProcessResult execute(std::span<const std::string_view> arguments);

}  // namespace retrace::process
