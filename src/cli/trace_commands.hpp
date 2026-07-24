#pragma once

// Internal command handlers for reading traces. They are separated from
// commands.cpp so trace inspection/validation can evolve independently.

#include <iosfwd>
#include <span>
#include <string_view>

namespace retrace::cli {

[[nodiscard]] int inspect_trace(std::span<const std::string_view> arguments,
                                std::ostream& output, std::ostream& error);
[[nodiscard]] int validate_trace(std::span<const std::string_view> arguments,
                                 std::ostream& output, std::ostream& error);

}  // namespace retrace::cli
