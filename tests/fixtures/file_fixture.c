// Target-side file-operation fixture for the injected runtime. Each mode runs a
// fixed sequence of libc file calls and checks what the target itself must still
// observe, so a hook that changed a return value or clobbered errno fails the
// run instead of quietly producing plausible-looking trace evidence.

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
  // One byte past the runtime's recorded path bound, so the attempt is reported
  // with a truncated path and the truncation flag.
  oversized_path_length = 4097,
  path_capacity = 512,
  concurrent_thread_count = 4,
  operations_per_thread = 8,
  // An errno value no call below can produce. Finding it unchanged proves a
  // successful hook left the caller's errno alone.
  errno_sentinel = 12345,
};

static const char observed_file_name[] = "observed.txt";

// Composes DIRECTORY/NAME and reports truncation rather than silently probing a
// shorter path than the caller asked for.
static int join_path(char* const destination, const size_t capacity,
                     const char* const directory, const char* const name) {
  const int written = snprintf(destination, capacity, "%s/%s", directory, name);
  return written > 0 && (size_t)written < capacity ? 0 : -1;
}

static int create_observed_file(const char* const path) {
  const int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    return -1;
  }
  return close(descriptor) == 0 ? 0 : -1;
}

// A successful open must return a usable descriptor and leave errno untouched.
static int expect_successful_open(const char* const path) {
  errno = errno_sentinel;
  const int descriptor = open(path, O_RDONLY);
  if (descriptor < 0 || errno != errno_sentinel) {
    return -1;
  }

  errno = errno_sentinel;
  if (close(descriptor) != 0 || errno != errno_sentinel) {
    return -1;
  }
  return 0;
}

// A failing call must return -1 and report the errno the kernel produced, not a
// value the interposed hook overwrote with its own bookkeeping.
static int expect_failing_open(const char* const path, const int expected_errno) {
  errno = 0;
  const int descriptor = open(path, O_RDONLY);
  if (descriptor >= 0) {
    (void)close(descriptor);
    return -1;
  }
  return errno == expected_errno ? 0 : -1;
}

static int expect_openat(const char* const directory) {
  const int directory_descriptor = open(directory, O_RDONLY | O_DIRECTORY);
  if (directory_descriptor < 0) {
    return -1;
  }

  errno = errno_sentinel;
  const int descriptor = openat(directory_descriptor, observed_file_name, O_RDONLY);
  const int failed = descriptor < 0 || errno != errno_sentinel;
  if (descriptor >= 0) {
    (void)close(descriptor);
  }
  (void)close(directory_descriptor);
  return failed ? -1 : 0;
}

// A path longer than the runtime records exercises the truncation flag. The
// kernel rejects it with ENAMETOOLONG, and the attempt is still reported.
static int expect_oversized_path_rejected(void) {
  char* const path = malloc((size_t)oversized_path_length + 1U);
  if (path == NULL) {
    return -1;
  }
  path[0] = '/';
  memset(path + 1, 'a', (size_t)oversized_path_length - 1U);
  path[oversized_path_length] = '\0';

  const int result = expect_failing_open(path, ENAMETOOLONG);
  free(path);
  return result;
}

static int expect_close_of_invalid_descriptor(void) {
  errno = 0;
  // Descriptor -1 is never valid, so this reaches the close hook and must come
  // back as the kernel's own failure.
  return close(-1) == 0 || errno != EBADF ? -1 : 0;
}

static int run_operations(const char* const directory) {
  char observed_path[path_capacity];
  char missing_path[path_capacity];
  if (join_path(observed_path, sizeof(observed_path), directory, observed_file_name) !=
          0 ||
      join_path(missing_path, sizeof(missing_path), directory, "absent.txt") != 0) {
    return EXIT_FAILURE;
  }

  if (create_observed_file(observed_path) != 0 ||
      expect_successful_open(observed_path) != 0 ||
      expect_failing_open(missing_path, ENOENT) != 0 || expect_openat(directory) != 0 ||
      expect_oversized_path_rejected() != 0 ||
      expect_close_of_invalid_descriptor() != 0) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

struct thread_arguments {
  const char* path;
  int failed;
};

// Concurrent opens exercise the hooks' thread-local recursion guard and the
// atomic sequence counter. Each thread reports through the shared argument
// object, which it alone writes.
static void* repeat_operations(void* const argument) {
  struct thread_arguments* const arguments = argument;
  for (int index = 0; index < operations_per_thread; ++index) {
    if (expect_successful_open(arguments->path) != 0) {
      arguments->failed = 1;
      return NULL;
    }
  }
  return NULL;
}

static int run_threads(const char* const directory) {
  char observed_path[path_capacity];
  if (join_path(observed_path, sizeof(observed_path), directory, observed_file_name) !=
      0) {
    return EXIT_FAILURE;
  }
  if (create_observed_file(observed_path) != 0) {
    return EXIT_FAILURE;
  }

  pthread_t threads[concurrent_thread_count];
  struct thread_arguments arguments[concurrent_thread_count];
  int started = 0;
  for (int index = 0; index < concurrent_thread_count; ++index) {
    arguments[index].path = observed_path;
    arguments[index].failed = 0;
    if (pthread_create(&threads[index], NULL, repeat_operations, &arguments[index]) !=
        0) {
      break;
    }
    ++started;
  }

  int failed = started != concurrent_thread_count;
  for (int index = 0; index < started; ++index) {
    (void)pthread_join(threads[index], NULL);
    failed = failed || arguments[index].failed != 0;
  }
  return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(const int argument_count, char* const argument_values[]) {
  if (argument_count != 3) {
    (void)fputs("usage: file_fixture (operations|threads) DIRECTORY\n", stderr);
    return EXIT_FAILURE;
  }

  const char* const mode = argument_values[1];
  const char* const directory = argument_values[2];
  if (strcmp(mode, "operations") == 0) {
    return run_operations(directory);
  }
  if (strcmp(mode, "threads") == 0) {
    return run_threads(directory);
  }

  (void)fputs("file_fixture: unknown mode\n", stderr);
  return EXIT_FAILURE;
}
