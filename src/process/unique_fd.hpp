#pragma once

// Minimal RAII owner for a Linux file descriptor. Copying is forbidden because
// two owners would double-close; moving transfers the integer and its lifetime.

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
    // close(2) errors cannot be usefully reported from a destructor. Operations
    // needing an observed close result use a higher-level explicit finish API.
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
    descriptor_ = replacement;
  }

 private:
  int descriptor_ = -1;
};

}  // namespace retrace::process
