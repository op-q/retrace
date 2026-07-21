#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <system_error>
#include <utility>

#include "unique_fd.hpp"

namespace retrace::process {

class Pipe final {
 public:
  Pipe() = default;

  Pipe(const Pipe&) = delete;
  Pipe& operator=(const Pipe&) = delete;

  Pipe(Pipe&&) noexcept = default;
  Pipe& operator=(Pipe&&) noexcept = default;

  [[nodiscard]] static std::error_code create(Pipe& result) {
    std::array<int, 2> descriptors{};
    if (::pipe2(descriptors.data(), O_CLOEXEC) < 0) {
      return {errno, std::generic_category()};
    }

    Pipe pipe;
    pipe.read_end_.reset(descriptors[0]);
    pipe.write_end_.reset(descriptors[1]);

    if (const auto error = move_above_standard_descriptors(pipe.read_end_)) {
      return error;
    }
    if (const auto error = move_above_standard_descriptors(pipe.write_end_)) {
      return error;
    }

    result = std::move(pipe);
    return {};
  }

  [[nodiscard]] int read_descriptor() const noexcept { return read_end_.get(); }
  [[nodiscard]] int write_descriptor() const noexcept { return write_end_.get(); }

  void close_read_end() noexcept { read_end_.reset(); }
  void close_write_end() noexcept { write_end_.reset(); }

 private:
  [[nodiscard]] static std::error_code move_above_standard_descriptors(
      UniqueFd& descriptor) {
    if (descriptor.get() > STDERR_FILENO) {
      return {};
    }

    int result = -1;
    do {
      result = ::fcntl(descriptor.get(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
      return {errno, std::generic_category()};
    }

    descriptor.reset(result);
    return {};
  }

  UniqueFd read_end_;
  UniqueFd write_end_;
};

}  // namespace retrace::process
