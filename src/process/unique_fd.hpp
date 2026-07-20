#pragma once

#include <unistd.h>

#include <utility>

namespace retrace::process {

class UniqueFd final {
 public:
  UniqueFd() = default;
  explicit UniqueFd(const int descriptor) noexcept : descriptor_(descriptor) {}

  ~UniqueFd() { reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] explicit operator bool() const noexcept { return descriptor_ >= 0; }

  [[nodiscard]] int release() noexcept { return std::exchange(descriptor_, -1); }

  void reset(const int replacement = -1) noexcept {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
    descriptor_ = replacement;
  }

 private:
  int descriptor_ = -1;
};

}  // namespace retrace::process
