// Linux process supervisor: owns fork/exec, process groups, stream collection,
// signal forwarding, runtime IPC, lifecycle events, and final wait status.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "retrace/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "pipe.hpp"
#include "retrace/runtime_protocol.h"
#include "runtime_channel.hpp"
#include "unique_fd.hpp"

namespace retrace::process {
namespace {

[[nodiscard]] std::error_code system_error(const int error_number) {
  return {error_number, std::generic_category()};
}

[[nodiscard]] bool is_preload_separator(const char byte) {
  return byte == ':' || byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' ||
         byte == '\f' || byte == '\v';
}

void build_target_environment(const int runtime_descriptor,
                              const std::string_view runtime_library,
                              std::vector<std::string>& owned_environment,
                              std::vector<char*>& environment_pointers) {
  // Build all strings before the char* array so vector growth cannot invalidate
  // pointers. RETRACE replaces its channel value and, when injecting, prepends
  // its library while retaining every caller-provided preload token.
  const std::string runtime_prefix = std::string{RETRACE_RUNTIME_EVENT_FD_ENV} + '=';
  constexpr std::string_view preload_prefix = "LD_PRELOAD=";
  std::string inherited_preloads;

  if (environ != nullptr) {
    for (auto cursor = environ; *cursor != nullptr; ++cursor) {
      const std::string_view entry{*cursor};
      if (entry.starts_with(runtime_prefix)) {
        continue;
      }
      if (!runtime_library.empty() && entry.starts_with(preload_prefix)) {
        const auto value = entry.substr(preload_prefix.size());
        if (!value.empty()) {
          if (!inherited_preloads.empty()) {
            inherited_preloads.push_back(':');
          }
          inherited_preloads.append(value);
        }
        continue;
      }
      owned_environment.emplace_back(entry);
    }
  }
  if (runtime_descriptor >= 0) {
    owned_environment.push_back(runtime_prefix + std::to_string(runtime_descriptor));
  }
  if (!runtime_library.empty()) {
    std::string preload{preload_prefix};
    preload.append(runtime_library);
    if (!inherited_preloads.empty()) {
      preload.push_back(':');
      preload.append(inherited_preloads);
    }
    owned_environment.push_back(std::move(preload));
  }

  environment_pointers.reserve(owned_environment.size() + 1U);
  for (auto& entry : owned_environment) {
    environment_pointers.push_back(entry.data());
  }
  environment_pointers.push_back(nullptr);
}

// Signals are blocked and consumed through signalfd, turning asynchronous
// delivery into an ordinary pollable descriptor. The original mask is RAII
// state that must be restored on every parent/child exit path.
class ForwardedSignals final {
 public:
  ForwardedSignals() = default;
  ~ForwardedSignals() { [[maybe_unused]] const auto restore_error = restore_mask(); }

  ForwardedSignals(const ForwardedSignals&) = delete;
  ForwardedSignals& operator=(const ForwardedSignals&) = delete;

  [[nodiscard]] std::error_code start() {
    if (::sigemptyset(&signals_) < 0 || ::sigaddset(&signals_, SIGINT) < 0 ||
        ::sigaddset(&signals_, SIGTERM) < 0) {
      return system_error(errno);
    }
    if (::sigprocmask(SIG_BLOCK, &signals_, &previous_mask_) < 0) {
      return system_error(errno);
    }
    mask_changed_ = true;

    const int descriptor = ::signalfd(-1, &signals_, SFD_CLOEXEC | SFD_NONBLOCK);
    if (descriptor < 0) {
      const auto create_error = system_error(errno);
      [[maybe_unused]] const auto restore_error = restore_mask();
      return create_error;
    }
    descriptor_.reset(descriptor);
    return {};
  }

  [[nodiscard]] std::error_code restore_mask() noexcept {
    if (!mask_changed_) {
      return {};
    }
    mask_changed_ = false;
    if (::sigprocmask(SIG_SETMASK, &previous_mask_, nullptr) < 0) {
      return system_error(errno);
    }
    return {};
  }

  [[nodiscard]] int descriptor() const noexcept { return descriptor_.get(); }

 private:
  sigset_t signals_{};
  sigset_t previous_mask_{};
  UniqueFd descriptor_;
  bool mask_changed_ = false;
};

void send_launch_error(const int descriptor, const int error_number) noexcept {
  // The close-on-exec launch pipe is a tiny parent/child protocol: either the
  // child writes one errno, or successful exec closes the descriptor at EOF.
  const auto* bytes = reinterpret_cast<const char*>(&error_number);
  std::size_t bytes_written = 0;

  while (bytes_written < sizeof(error_number)) {
    const auto result = ::write(descriptor, bytes + bytes_written,
                                sizeof(error_number) - bytes_written);
    if (result > 0) {
      bytes_written += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return;
  }
}

[[nodiscard]] int redirect_descriptor(const int source,
                                      const int destination) noexcept {
  if (source == destination) {
    int flags = -1;
    do {
      flags = ::fcntl(source, F_GETFD);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0) {
      return errno;
    }

    int result = -1;
    do {
      result = ::fcntl(source, F_SETFD, flags & ~FD_CLOEXEC);
    } while (result < 0 && errno == EINTR);
    return result < 0 ? errno : 0;
  }

  int result = -1;
  do {
    result = ::dup2(source, destination);
  } while (result < 0 && errno == EINTR);
  return result < 0 ? errno : 0;
}

struct LaunchErrorRead {
  int error_number = 0;
  std::size_t bytes_read = 0;
  std::error_code error;
};

[[nodiscard]] LaunchErrorRead read_launch_error(const int descriptor) {
  LaunchErrorRead result;
  auto* bytes = reinterpret_cast<char*>(&result.error_number);

  while (result.bytes_read < sizeof(result.error_number)) {
    const auto read_result = ::read(descriptor, bytes + result.bytes_read,
                                    sizeof(result.error_number) - result.bytes_read);
    if (read_result > 0) {
      result.bytes_read += static_cast<std::size_t>(read_result);
      continue;
    }
    if (read_result == 0) {
      return result;
    }
    if (errno == EINTR) {
      continue;
    }
    result.error = system_error(errno);
    return result;
  }

  return result;
}

[[nodiscard]] std::error_code wait_for_child(const pid_t child, int& wait_status) {
  pid_t result = -1;
  do {
    result = ::waitpid(child, &wait_status, 0);
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    return system_error(errno);
  }
  return {};
}

struct MonitoredStream {
  Pipe* pipe = nullptr;
  ProcessEventType event_type = ProcessEventType::standard_output;
};

struct CaptureResult {
  int wait_status = 0;
  std::error_code output_error;
  std::error_code runtime_error;
  std::error_code signal_error;
  std::error_code wait_error;
  bool runtime_handshake_received = false;
};

// A handler failure stops future callback delivery but never abandons the
// child; supervision continues until streams are drained and the PID reaped.
class EventDispatcher final {
 public:
  EventDispatcher(const int process_id, const ProcessEventHandler& handler)
      : process_id_(process_id), handler_(handler) {}

  void send(const ProcessEventType type, const int value = 0,
            const std::string_view bytes = {}) {
    if (!handler_ || error_) {
      return;
    }

    try {
      error_ = handler_(
          {.type = type, .process_id = process_id_, .value = value, .bytes = bytes});
    } catch (const std::system_error& error) {
      error_ = error.code();
    } catch (...) {
      error_ = std::make_error_code(std::errc::io_error);
    }
  }

  [[nodiscard]] const std::error_code& error() const noexcept { return error_; }

 private:
  int process_id_ = 0;
  const ProcessEventHandler& handler_;
  std::error_code error_;
};

[[nodiscard]] std::error_code receive_runtime_messages(RuntimeChannel& runtime_channel,
                                                       EventDispatcher& events,
                                                       bool& handshake_received) {
  // Drain only what is immediately queued. A second handshake is invalid in the
  // current session-level protocol and closes the instrumentation channel.
  while (runtime_channel.supervisor_descriptor() >= 0) {
    const auto received = runtime_channel.receive();
    switch (received.status) {
      case RuntimeReceiveStatus::handshake:
        if (handshake_received) {
          return std::make_error_code(std::errc::protocol_error);
        }
        handshake_received = true;
        events.send(ProcessEventType::runtime_handshake);
        break;
      case RuntimeReceiveStatus::would_block:
        return {};
      case RuntimeReceiveStatus::closed:
        runtime_channel.close_supervisor_end();
        return {};
      case RuntimeReceiveStatus::error:
        return received.error;
    }
  }
  return {};
}

struct ChildPollResult {
  int wait_status = 0;
  bool reaped = false;
  std::error_code error;
};

[[nodiscard]] std::error_code forward_pending_signals(const int signal_descriptor,
                                                      EventDispatcher& events,
                                                      const pid_t process_group) {
  while (true) {
    signalfd_siginfo information{};
    ssize_t read_result = -1;
    do {
      read_result = ::read(signal_descriptor, &information, sizeof(information));
    } while (read_result < 0 && errno == EINTR);

    if (read_result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return {};
    }
    if (read_result < 0) {
      return system_error(errno);
    }
    if (read_result != static_cast<ssize_t>(sizeof(information))) {
      return std::make_error_code(std::errc::protocol_error);
    }

    const auto signal_number = static_cast<int>(information.ssi_signo);
    if (signal_number != SIGINT && signal_number != SIGTERM) {
      return std::make_error_code(std::errc::protocol_error);
    }

    int kill_result = -1;
    do {
      kill_result = ::kill(-process_group, signal_number);
    } while (kill_result < 0 && errno == EINTR);

    if (kill_result == 0) {
      events.send(ProcessEventType::signal_forwarded, signal_number);
    } else if (errno != ESRCH) {
      return system_error(errno);
    }
  }
}

[[nodiscard]] ChildPollResult poll_for_child(const pid_t child) {
  ChildPollResult poll;
  pid_t result = -1;
  do {
    result = ::waitpid(child, &poll.wait_status, WNOHANG);
  } while (result < 0 && errno == EINTR);

  if (result < 0) {
    poll.error = system_error(errno);
    return poll;
  }
  poll.reaped = result == child;
  return poll;
}

[[nodiscard]] CaptureResult capture_output_and_wait(const pid_t child,
                                                    Pipe& standard_output,
                                                    Pipe& standard_error,
                                                    RuntimeChannel& runtime_channel,
                                                    const int signal_descriptor,
                                                    EventDispatcher& events) {
  // stdout, stderr, signals, runtime packets, and child completion must progress
  // together. Polling avoids the classic deadlock where a child fills one pipe
  // while the parent blocks reading or waiting on something else.
  constexpr std::size_t stream_count = 2U;
  constexpr std::size_t signal_descriptor_index = stream_count;
  constexpr std::size_t runtime_descriptor_index = stream_count + 1U;
  constexpr std::size_t descriptor_count = stream_count + 2U;
  constexpr int child_poll_interval_milliseconds = 50;
  std::array<MonitoredStream, stream_count> streams{
      MonitoredStream{&standard_output, ProcessEventType::standard_output},
      MonitoredStream{&standard_error, ProcessEventType::standard_error}};
  std::array<char, std::size_t{16U} * 1024U> buffer{};
  CaptureResult result;
  std::size_t first_stream = 0U;
  bool child_reaped = false;

  while (!child_reaped) {
    if (const auto error = forward_pending_signals(signal_descriptor, events, child)) {
      result.signal_error = error;
      break;
    }

    const auto child_poll = poll_for_child(child);
    if (child_poll.error) {
      result.wait_error = child_poll.error;
      break;
    }
    child_reaped = child_poll.reaped;
    if (child_reaped) {
      result.wait_status = child_poll.wait_status;
      break;
    }

    std::array<pollfd, descriptor_count> descriptors{};
    for (std::size_t index = 0; index < stream_count; ++index) {
      descriptors[index].fd = streams[index].pipe->read_descriptor();
      descriptors[index].events = POLLIN;
    }
    descriptors[signal_descriptor_index].fd = signal_descriptor;
    descriptors[signal_descriptor_index].events = POLLIN;
    descriptors[runtime_descriptor_index].fd = runtime_channel.supervisor_descriptor();
    descriptors[runtime_descriptor_index].events = POLLIN;

    int poll_result = -1;
    do {
      poll_result = ::poll(descriptors.data(), descriptors.size(),
                           child_poll_interval_milliseconds);
    } while (poll_result < 0 && errno == EINTR);

    if (poll_result < 0) {
      result.output_error = system_error(errno);
      break;
    }

    if ((descriptors[signal_descriptor_index].revents & POLLIN) != 0) {
      if (const auto error =
              forward_pending_signals(signal_descriptor, events, child)) {
        result.signal_error = error;
        break;
      }
    } else if (descriptors[signal_descriptor_index].revents != 0) {
      result.signal_error = std::make_error_code(std::errc::io_error);
      break;
    }

    const auto runtime_events = descriptors[runtime_descriptor_index].revents;
    if (runtime_events != 0 && runtime_channel.supervisor_descriptor() >= 0) {
      if ((runtime_events & POLLNVAL) != 0) {
        result.runtime_error = std::make_error_code(std::errc::bad_file_descriptor);
        runtime_channel.close_supervisor_end();
      } else if ((runtime_events & (POLLIN | POLLHUP)) != 0) {
        if (const auto error = receive_runtime_messages(
                runtime_channel, events, result.runtime_handshake_received)) {
          result.runtime_error = error;
          runtime_channel.close_supervisor_end();
        }
      } else {
        result.runtime_error = std::make_error_code(std::errc::io_error);
        runtime_channel.close_supervisor_end();
      }
    }

    const auto poll_first_stream = first_stream;
    auto next_first_stream = first_stream;
    bool captured_chunk = false;
    for (std::size_t offset = 0; offset < stream_count; ++offset) {
      const auto index = (poll_first_stream + offset) % stream_count;
      const auto ready_events = descriptors[index].revents;
      if (ready_events == 0) {
        continue;
      }

      auto& monitored = streams[index];
      if ((ready_events & POLLNVAL) != 0) {
        if (!result.output_error) {
          result.output_error = std::make_error_code(std::errc::bad_file_descriptor);
        }
        monitored.pipe->close_read_end();
        continue;
      }
      if ((ready_events & (POLLIN | POLLHUP)) == 0) {
        if (!result.output_error) {
          result.output_error = std::make_error_code(std::errc::io_error);
        }
        monitored.pipe->close_read_end();
        continue;
      }

      ssize_t read_result = -1;
      do {
        read_result =
            ::read(monitored.pipe->read_descriptor(), buffer.data(), buffer.size());
      } while (read_result < 0 && errno == EINTR);

      if (read_result == 0) {
        monitored.pipe->close_read_end();
        continue;
      }
      if (read_result < 0) {
        if (!result.output_error) {
          result.output_error = system_error(errno);
        }
        monitored.pipe->close_read_end();
        continue;
      }

      if (!captured_chunk) {
        next_first_stream = (index + 1U) % stream_count;
        captured_chunk = true;
      }
      events.send(
          monitored.event_type, 0,
          std::string_view{buffer.data(), static_cast<std::size_t>(read_result)});
    }
    first_stream = next_first_stream;
  }

  if (child_reaped) {
    // A descendant may inherit either write end. Snapshot the bytes already in
    // each pipe when the direct child exits, rather than waiting for inherited
    // descriptors to close or allowing a descendant to extend the drain forever.
    for (auto& monitored : streams) {
      if (monitored.pipe->read_descriptor() < 0) {
        continue;
      }

      int bytes_available = 0;
      int ioctl_result = -1;
      do {
        ioctl_result =
            ::ioctl(monitored.pipe->read_descriptor(), FIONREAD, &bytes_available);
      } while (ioctl_result < 0 && errno == EINTR);

      if (ioctl_result < 0 || bytes_available < 0) {
        if (!result.output_error) {
          result.output_error = ioctl_result < 0
                                    ? system_error(errno)
                                    : std::make_error_code(std::errc::protocol_error);
        }
        monitored.pipe->close_read_end();
        continue;
      }

      auto bytes_remaining = static_cast<std::size_t>(bytes_available);
      while (bytes_remaining > 0U) {
        const auto bytes_to_read = std::min(buffer.size(), bytes_remaining);
        ssize_t read_result = -1;
        do {
          read_result =
              ::read(monitored.pipe->read_descriptor(), buffer.data(), bytes_to_read);
        } while (read_result < 0 && errno == EINTR);

        if (read_result <= 0) {
          if (!result.output_error) {
            result.output_error = read_result < 0
                                      ? system_error(errno)
                                      : std::make_error_code(std::errc::protocol_error);
          }
          break;
        }

        const auto bytes_read = static_cast<std::size_t>(read_result);
        bytes_remaining -= bytes_read;
        events.send(monitored.event_type, 0,
                    std::string_view{buffer.data(), bytes_read});
      }
      monitored.pipe->close_read_end();
    }

    if (runtime_channel.supervisor_descriptor() >= 0) {
      if (const auto error = receive_runtime_messages(
              runtime_channel, events, result.runtime_handshake_received)) {
        result.runtime_error = error;
      }
      runtime_channel.close_supervisor_end();
    }
  } else {
    standard_output.close_read_end();
    standard_error.close_read_end();
    runtime_channel.close_supervisor_end();
  }

  if (!child_reaped && !result.wait_error) {
    result.wait_error = wait_for_child(child, result.wait_status);
  }

  return result;
}

}  // namespace

ProcessResult execute(const std::span<const std::string_view> arguments,
                      const ProcessEventHandler& event_handler,
                      const ExecuteOptions& options) {
  // Anything requiring allocation or C++ containers is prepared before fork.
  // The child then follows a small, allocation-free path into execvpe.
  if (arguments.empty() ||
      std::any_of(arguments.begin(), arguments.end(),
                  [](const auto argument) {
                    return argument.find('\0') != std::string_view::npos;
                  }) ||
      options.working_directory.find('\0') != std::string_view::npos ||
      options.runtime_library.find('\0') != std::string_view::npos ||
      (!options.runtime_library.empty() &&
       (options.runtime_library.front() != '/' ||
        std::any_of(options.runtime_library.begin(), options.runtime_library.end(),
                    is_preload_separator))) ||
      (!options.runtime_channel_enabled && !options.runtime_library.empty())) {
    return {.state = ProcessState::supervisor_failed,
            .error = std::make_error_code(std::errc::invalid_argument)};
  }

  std::vector<std::string> owned_arguments;
  owned_arguments.reserve(arguments.size());
  for (const auto argument : arguments) {
    owned_arguments.emplace_back(argument);
  }

  std::vector<char*> argument_pointers;
  argument_pointers.reserve(owned_arguments.size() + 1U);
  for (auto& argument : owned_arguments) {
    argument_pointers.push_back(argument.data());
  }
  argument_pointers.push_back(nullptr);

  const std::string owned_working_directory{options.working_directory};

  Pipe launch_errors;
  if (const auto error = Pipe::create(launch_errors)) {
    return {.state = ProcessState::supervisor_failed, .error = error};
  }
  Pipe standard_output;
  if (const auto error = Pipe::create(standard_output)) {
    return {.state = ProcessState::supervisor_failed, .error = error};
  }
  Pipe standard_error;
  if (const auto error = Pipe::create(standard_error)) {
    return {.state = ProcessState::supervisor_failed, .error = error};
  }
  RuntimeChannel runtime_channel;
  if (options.runtime_channel_enabled) {
    if (const auto error = RuntimeChannel::create(runtime_channel)) {
      return {.state = ProcessState::supervisor_failed, .error = error};
    }
  }

  std::vector<std::string> owned_environment;
  std::vector<char*> environment_pointers;
  build_target_environment(runtime_channel.target_descriptor(), options.runtime_library,
                           owned_environment, environment_pointers);

  ForwardedSignals forwarded_signals;
  if (const auto error = forwarded_signals.start()) {
    return {.state = ProcessState::supervisor_failed, .error = error};
  }

  const pid_t child = ::fork();
  if (child < 0) {
    return {.state = ProcessState::supervisor_failed, .error = system_error(errno)};
  }

  if (child == 0) {
    launch_errors.close_read_end();
    standard_output.close_read_end();
    standard_error.close_read_end();
    runtime_channel.close_supervisor_end();

    if (::setpgid(0, 0) < 0) {
      send_launch_error(launch_errors.write_descriptor(), errno);
      ::_exit(127);
    }
    if (const auto error = forwarded_signals.restore_mask()) {
      send_launch_error(launch_errors.write_descriptor(), error.value());
      ::_exit(127);
    }
    if (!owned_working_directory.empty() &&
        ::chdir(owned_working_directory.c_str()) < 0) {
      send_launch_error(launch_errors.write_descriptor(), errno);
      ::_exit(127);
    }

    if (const int error =
            redirect_descriptor(standard_output.write_descriptor(), STDOUT_FILENO);
        error != 0) {
      send_launch_error(launch_errors.write_descriptor(), error);
      ::_exit(127);
    }
    if (const int error =
            redirect_descriptor(standard_error.write_descriptor(), STDERR_FILENO);
        error != 0) {
      send_launch_error(launch_errors.write_descriptor(), error);
      ::_exit(127);
    }

    standard_output.close_write_end();
    standard_error.close_write_end();
    if (runtime_channel.target_descriptor() >= 0) {
      if (const int error = runtime_channel.make_target_descriptor_inheritable();
          error != 0) {
        send_launch_error(launch_errors.write_descriptor(), error);
        ::_exit(127);
      }
    }
    ::execvpe(argument_pointers.front(), argument_pointers.data(),
              environment_pointers.data());

    const int launch_error = errno;
    send_launch_error(launch_errors.write_descriptor(), launch_error);
    ::_exit(127);
  }

  launch_errors.close_write_end();
  standard_output.close_write_end();
  standard_error.close_write_end();
  runtime_channel.close_target_end();

  EventDispatcher events{static_cast<int>(child), event_handler};
  events.send(ProcessEventType::started);

  const auto launch_error = read_launch_error(launch_errors.read_descriptor());
  launch_errors.close_read_end();

  if (!launch_error.error && launch_error.bytes_read == 0U) {
    events.send(ProcessEventType::exec_succeeded);
  } else if (!launch_error.error &&
             launch_error.bytes_read == sizeof(launch_error.error_number)) {
    events.send(ProcessEventType::launch_failed, launch_error.error_number);
  }

  const auto capture =
      capture_output_and_wait(child, standard_output, standard_error, runtime_channel,
                              forwarded_signals.descriptor(), events);

  const auto signal_restore_error = forwarded_signals.restore_mask();

  if (capture.wait_error) {
    return {.state = ProcessState::supervisor_failed,
            .process_id = static_cast<int>(child),
            .error = capture.wait_error};
  }
  if (capture.signal_error) {
    return {.state = ProcessState::supervisor_failed,
            .process_id = static_cast<int>(child),
            .error = capture.signal_error};
  }
  if (signal_restore_error) {
    return {.state = ProcessState::supervisor_failed,
            .process_id = static_cast<int>(child),
            .error = signal_restore_error};
  }

  if (launch_error.error) {
    return {.state = ProcessState::supervisor_failed,
            .process_id = static_cast<int>(child),
            .error = launch_error.error};
  }
  if (launch_error.bytes_read != 0U) {
    if (launch_error.bytes_read == sizeof(launch_error.error_number)) {
      if (events.error()) {
        return {.state = ProcessState::supervisor_failed,
                .process_id = static_cast<int>(child),
                .error = events.error()};
      }
      return {.state = ProcessState::launch_failed,
              .process_id = static_cast<int>(child),
              .error = system_error(launch_error.error_number)};
    }
    return {.state = ProcessState::supervisor_failed,
            .process_id = static_cast<int>(child),
            .error = std::make_error_code(std::errc::protocol_error)};
  }
  if (capture.output_error) {
    return {.state = ProcessState::supervisor_failed,
            .process_id = static_cast<int>(child),
            .error = capture.output_error};
  }
  if (events.error()) {
    return {.state = ProcessState::supervisor_failed,
            .process_id = static_cast<int>(child),
            .error = events.error()};
  }

  if (WIFEXITED(capture.wait_status)) {
    const auto exit_code = WEXITSTATUS(capture.wait_status);
    events.send(ProcessEventType::exited, exit_code);
    if (capture.runtime_error) {
      return {.state = ProcessState::supervisor_failed,
              .process_id = static_cast<int>(child),
              .error = capture.runtime_error};
    }
    if (events.error()) {
      return {.state = ProcessState::supervisor_failed,
              .process_id = static_cast<int>(child),
              .error = events.error()};
    }
    if (!options.runtime_library.empty() && !capture.runtime_handshake_received) {
      return {.state = ProcessState::runtime_unavailable,
              .exit_code = exit_code,
              .process_id = static_cast<int>(child),
              .error = std::make_error_code(std::errc::not_supported)};
    }
    return {.state = ProcessState::exited,
            .exit_code = exit_code,
            .process_id = static_cast<int>(child),
            .error = {}};
  }
  if (WIFSIGNALED(capture.wait_status)) {
    const auto signal_number = WTERMSIG(capture.wait_status);
    events.send(ProcessEventType::signaled, signal_number);
    if (capture.runtime_error) {
      return {.state = ProcessState::supervisor_failed,
              .process_id = static_cast<int>(child),
              .error = capture.runtime_error};
    }
    if (events.error()) {
      return {.state = ProcessState::supervisor_failed,
              .process_id = static_cast<int>(child),
              .error = events.error()};
    }
    if (!options.runtime_library.empty() && !capture.runtime_handshake_received) {
      return {.state = ProcessState::runtime_unavailable,
              .signal_number = signal_number,
              .process_id = static_cast<int>(child),
              .error = std::make_error_code(std::errc::not_supported)};
    }
    return {.state = ProcessState::signaled,
            .signal_number = signal_number,
            .process_id = static_cast<int>(child),
            .error = {}};
  }

  return {.state = ProcessState::supervisor_failed,
          .process_id = static_cast<int>(child),
          .error = std::make_error_code(std::errc::protocol_error)};
}

}  // namespace retrace::process
