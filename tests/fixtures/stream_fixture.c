#include <stdio.h>
#include <stdlib.h>

int main(void) {
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
