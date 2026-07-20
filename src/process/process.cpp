#include "retrace/process.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <string>
#include <system_error>
#include <vector>

#include "unique_fd.hpp"

namespace retrace::process {
namespace {

[[nodiscard]] std::error_code system_error(const int error_number) {
  return {error_number, std::generic_category()};
}

[[nodiscard]] std::error_code set_close_on_exec(const int descriptor) {
  int flags = -1;
  do {
    flags = ::fcntl(descriptor, F_GETFD);
  } while (flags < 0 && errno == EINTR);

  if (flags < 0) {
    return system_error(errno);
  }

  int result = -1;
  do {
    result = ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
  } while (result < 0 && errno == EINTR);

  if (result < 0) {
    return system_error(errno);
  }

  return {};
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

}  // namespace

ProcessResult execute(const std::span<const std::string_view> arguments) {
  if (arguments.empty()) {
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

  std::array<int, 2> raw_pipe{};
  if (::pipe(raw_pipe.data()) < 0) {
    return {.state = ProcessState::supervisor_failed, .error = system_error(errno)};
  }

  UniqueFd read_end{raw_pipe[0]};
  UniqueFd write_end{raw_pipe[1]};

  if (const auto error = set_close_on_exec(read_end.get())) {
    return {.state = ProcessState::supervisor_failed, .error = error};
  }
  if (const auto error = set_close_on_exec(write_end.get())) {
    return {.state = ProcessState::supervisor_failed, .error = error};
  }

  const pid_t child = ::fork();
  if (child < 0) {
    return {.state = ProcessState::supervisor_failed, .error = system_error(errno)};
  }

  if (child == 0) {
    read_end.reset();
    ::execvp(argument_pointers.front(), argument_pointers.data());

    const int launch_error = errno;
    send_launch_error(write_end.get(), launch_error);
    ::_exit(127);
  }

  write_end.reset();
  const auto launch_error = read_launch_error(read_end.get());
  read_end.reset();

  int wait_status = 0;
  if (const auto error = wait_for_child(child, wait_status)) {
    return {.state = ProcessState::supervisor_failed, .error = error};
  }

  if (launch_error.error) {
    return {.state = ProcessState::supervisor_failed, .error = launch_error.error};
  }
  if (launch_error.bytes_read == sizeof(launch_error.error_number)) {
    return {.state = ProcessState::launch_failed,
            .error = system_error(launch_error.error_number)};
  }
  if (launch_error.bytes_read != 0U) {
    return {.state = ProcessState::supervisor_failed,
            .error = std::make_error_code(std::errc::protocol_error)};
  }

  if (WIFEXITED(wait_status)) {
    return {.state = ProcessState::exited,
            .exit_code = WEXITSTATUS(wait_status),
            .error = {}};
  }
  if (WIFSIGNALED(wait_status)) {
    return {.state = ProcessState::signaled,
            .signal_number = WTERMSIG(wait_status),
            .error = {}};
  }

  return {.state = ProcessState::supervisor_failed,
          .error = std::make_error_code(std::errc::protocol_error)};
}

}  // namespace retrace::process
