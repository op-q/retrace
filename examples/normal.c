#include <stdio.h>
#include <stdlib.h>

int main(void) {
  if (fputs("normal example: stdout\n", stdout) == EOF || fflush(stdout) == EOF) {
    return EXIT_FAILURE;
  }
  if (fputs("normal example: stderr\n", stderr) == EOF || fflush(stderr) == EOF) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
