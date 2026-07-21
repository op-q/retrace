#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  large_chunk_size = 4096,
  large_chunk_count = 32,
};

static int write_small_output(void) {
  if (fputs("fixture: stdout\n", stdout) == EOF) {
    return EXIT_FAILURE;
  }
  if (fflush(stdout) == EOF) {
    return EXIT_FAILURE;
  }

  if (fputs("fixture: stderr\n", stderr) == EOF) {
    return EXIT_FAILURE;
  }
  if (fflush(stderr) == EOF) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

static int write_large_output(void) {
  char stdout_chunk[large_chunk_size];
  char stderr_chunk[large_chunk_size];
  memset(stdout_chunk, 'o', sizeof(stdout_chunk));
  memset(stderr_chunk, 'e', sizeof(stderr_chunk));

  for (int index = 0; index < large_chunk_count; ++index) {
    if (fwrite(stdout_chunk, sizeof(stdout_chunk), 1, stdout) != 1) {
      return EXIT_FAILURE;
    }
    if (fflush(stdout) == EOF) {
      return EXIT_FAILURE;
    }
    if (fwrite(stderr_chunk, sizeof(stderr_chunk), 1, stderr) != 1) {
      return EXIT_FAILURE;
    }
    if (fflush(stderr) == EOF) {
      return EXIT_FAILURE;
    }
  }

  return EXIT_SUCCESS;
}

int main(const int argc, char* argv[]) {
  if (argc == 1) {
    return write_small_output();
  }
  if (argc == 2 && strcmp(argv[1], "--large") == 0) {
    return write_large_output();
  }
  return EXIT_FAILURE;
}
