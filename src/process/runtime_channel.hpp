#pragma once

// Internal ownership and receive API for the live runtime IPC socket pair. Wire
// decoding remains private so callers can only observe validated message kinds.

#include <cstdint>
#include <system_error>

#include "retrace/process.hpp"
#include "unique_fd.hpp"

namespace retrace::process {

enum class RuntimeReceiveStatus : std::uint8_t {
  // `would_block` is an ordinary nonblocking state, while `closed` means the
  // target side has gone away after all queued packets were consumed.
  handshake,
  operation,
  would_block,
  closed,
  error,
};

struct RuntimeReceiveResult {
  RuntimeReceiveStatus status = RuntimeReceiveStatus::error;
  std::error_code error;
};

// Before fork this object owns both endpoints. After fork, each process closes
// its unused end; only the target endpoint is deliberately inherited across exec.
class RuntimeChannel final {
 public:
  RuntimeChannel() = default;

  RuntimeChannel(const RuntimeChannel&) = delete;
  RuntimeChannel& operator=(const RuntimeChannel&) = delete;

  RuntimeChannel(RuntimeChannel&&) noexcept = default;
  RuntimeChannel& operator=(RuntimeChannel&&) noexcept = default;

  [[nodiscard]] static std::error_code create(RuntimeChannel& result);

  [[nodiscard]] int supervisor_descriptor() const noexcept {
    return supervisor_end_.get();
  }
  [[nodiscard]] int target_descriptor() const noexcept { return target_end_.get(); }

  [[nodiscard]] int make_target_descriptor_inheritable() const noexcept;

  // `operation` is written only when the result is `operation`. The caller owns
  // and reuses one instance so a high-volume target does not reallocate the
  // path buffer for every recorded call.
  [[nodiscard]] RuntimeReceiveResult receive(RuntimeOperation& operation) const;

  void close_supervisor_end() noexcept { supervisor_end_.reset(); }
  void close_target_end() noexcept { target_end_.reset(); }

 private:
  UniqueFd supervisor_end_;
  UniqueFd target_end_;
};

}  // namespace retrace::process
