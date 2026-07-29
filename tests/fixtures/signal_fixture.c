// Process-group fixture for SIGINT/SIGTERM forwarding tests. It can create a
// child so the test proves a group-directed signal reaches more than one PID.

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t received_signal = 0;

static void remember_signal(const int signal_number) {
  received_signal = signal_number;
}

static int install_signal_handlers(void) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = remember_signal;
  if (sigemptyset(&action.sa_mask) < 0) {
    return -1;
  }
  if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
    return -1;
  }
  return 0;
}

static int expected_signal(const char* argument) {
  if (strcmp(argument, "--expect-int") == 0 || strcmp(argument, "--request-int") == 0) {
    return SIGINT;
  }
  if (strcmp(argument, "--expect-term") == 0 ||
      strcmp(argument, "--request-term") == 0) {
    return SIGTERM;
  }
  return 0;
}

int main(const int argc, char* argv[]) {
  if (argc != 2 || install_signal_handlers() < 0) {
    return EXIT_FAILURE;
  }

  const int expected = expected_signal(argv[1]);
  const pid_t leader = getpid();
  if (expected == 0 || getpgrp() != leader) {
    return EXIT_FAILURE;
  }

  const pid_t descendant = fork();
  if (descendant < 0) {
    return EXIT_FAILURE;
  }
  if (descendant == 0) {
    alarm(5U);
    if (getpgrp() != leader) {
      _exit(EXIT_FAILURE);
    }
    while (received_signal == 0) {
      pause();
    }
    _exit(received_signal == expected ? EXIT_SUCCESS : EXIT_FAILURE);
  }

  alarm(5U);
  if (fputs("ready\n", stdout) == EOF || fflush(stdout) == EOF) {
    return EXIT_FAILURE;
  }
  if (strncmp(argv[1], "--request-", 10U) == 0 && kill(getppid(), expected) < 0) {
    return EXIT_FAILURE;
  }
  while (received_signal == 0) {
    pause();
  }

  int wait_status = 0;
  pid_t wait_result = -1;
  do {
    wait_result = waitpid(descendant, &wait_status, 0);
  } while (wait_result < 0 && errno == EINTR);

  if (received_signal != expected || wait_result != descendant ||
      !WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  if (fputs("forwarded\n", stdout) == EOF || fflush(stdout) == EOF) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
