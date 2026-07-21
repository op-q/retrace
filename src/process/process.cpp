#include "retrace/process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
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

namespace retrace::process {
namespace {

[[nodiscard]] std::error_code system_error(const int error_number) {
  return {error_number, std::generic_category()};
}

void send_launch_error(const int descriptor, const int error_number) noexcept {
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
  std::error_code wait_error;
};

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

struct ChildPollResult {
  int wait_status = 0;
  bool reaped = false;
  std::error_code error;
};

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
                                                    EventDispatcher& events) {
  constexpr std::size_t stream_count = 2U;
  constexpr int child_poll_interval_milliseconds = 50;
  std::array<MonitoredStream, stream_count> streams{
      MonitoredStream{&standard_output, ProcessEventType::standard_output},
      MonitoredStream{&standard_error, ProcessEventType::standard_error}};
  std::array<char, std::size_t{16U} * 1024U> buffer{};
  CaptureResult result;
  std::size_t open_streams = stream_count;
  std::size_t first_stream = 0U;
  bool child_reaped = false;

  while (open_streams > 0U && !child_reaped) {
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

    std::array<pollfd, stream_count> descriptors{};
    for (std::size_t index = 0; index < stream_count; ++index) {
      descriptors[index].fd = streams[index].pipe->read_descriptor();
      descriptors[index].events = POLLIN;
    }

    int poll_result = -1;
    do {
      poll_result = ::poll(descriptors.data(), descriptors.size(),
                           child_poll_interval_milliseconds);
    } while (poll_result < 0 && errno == EINTR);

    if (poll_result < 0) {
      result.output_error = system_error(errno);
      break;
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
        --open_streams;
        continue;
      }
      if ((ready_events & (POLLIN | POLLHUP)) == 0) {
        if (!result.output_error) {
          result.output_error = std::make_error_code(std::errc::io_error);
        }
        monitored.pipe->close_read_end();
        --open_streams;
        continue;
      }

      ssize_t read_result = -1;
      do {
        read_result =
            ::read(monitored.pipe->read_descriptor(), buffer.data(), buffer.size());
      } while (read_result < 0 && errno == EINTR);

      if (read_result == 0) {
        monitored.pipe->close_read_end();
        --open_streams;
        continue;
      }
      if (read_result < 0) {
        if (!result.output_error) {
          result.output_error = system_error(errno);
        }
        monitored.pipe->close_read_end();
        --open_streams;
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
  } else {
    standard_output.close_read_end();
    standard_error.close_read_end();
  }

  if (!child_reaped && !result.wait_error) {
    result.wait_error = wait_for_child(child, result.wait_status);
  }

  return result;
}

}  // namespace

ProcessResult execute(const std::span<const std::string_view> arguments,
                      const ProcessEventHandler& event_handler) {
  if (arguments.empty() ||
      std::any_of(arguments.begin(), arguments.end(), [](const auto argument) {
        return argument.find('\0') != std::string_view::npos;
      })) {
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

  const pid_t child = ::fork();
  if (child < 0) {
    return {.state = ProcessState::supervisor_failed, .error = system_error(errno)};
  }

  if (child == 0) {
    launch_errors.close_read_end();
    standard_output.close_read_end();
    standard_error.close_read_end();

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
    ::execvp(argument_pointers.front(), argument_pointers.data());

    const int launch_error = errno;
    send_launch_error(launch_errors.write_descriptor(), launch_error);
    ::_exit(127);
  }

  launch_errors.close_write_end();
  standard_output.close_write_end();
  standard_error.close_write_end();

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
      capture_output_and_wait(child, standard_output, standard_error, events);

  if (capture.wait_error) {
    return {.state = ProcessState::supervisor_failed,
            .process_id = static_cast<int>(child),
            .error = capture.wait_error};
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
    if (events.error()) {
      return {.state = ProcessState::supervisor_failed,
              .process_id = static_cast<int>(child),
              .error = events.error()};
    }
    return {.state = ProcessState::exited,
            .exit_code = exit_code,
            .process_id = static_cast<int>(child),
            .error = {}};
  }
  if (WIFSIGNALED(capture.wait_status)) {
    const auto signal_number = WTERMSIG(capture.wait_status);
    events.send(ProcessEventType::signaled, signal_number);
    if (events.error()) {
      return {.state = ProcessState::supervisor_failed,
              .process_id = static_cast<int>(child),
              .error = events.error()};
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
