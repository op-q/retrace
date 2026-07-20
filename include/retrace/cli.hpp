#pragma once

#include <cstdint>
#include <iosfwd>
#include <span>
#include <string_view>

namespace retrace::cli {

enum class ExitCode : std::uint8_t {
  success = 0,
  internal_error = 1,
  usage_error = 2,
  target_launch_error = 5,
};

[[nodiscard]] int run(std::span<const std::string_view> arguments, std::ostream& output,
                      std::ostream& error);

}  // namespace retrace::cli
