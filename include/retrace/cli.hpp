#pragma once

// Public command-line boundary. The CLI accepts argument views and caller-owned
// streams, which keeps parsing and diagnostics independent from global stdio.

#include <cstdint>
#include <iosfwd>
#include <span>
#include <string_view>

namespace retrace::cli {

enum class ExitCode : std::uint8_t {
  // These values describe RETRACE failures. A successful `run` may instead
  // return the target program's own exit status.
  success = 0,
  internal_error = 1,
  usage_error = 2,
  trace_format_error = 4,
  target_launch_error = 5,
};

[[nodiscard]] int run(std::span<const std::string_view> arguments, std::ostream& output,
                      std::ostream& error);

}  // namespace retrace::cli
