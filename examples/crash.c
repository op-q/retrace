#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
  if (fputs("crash example: raising SIGSEGV\n", stderr) == EOF ||
      fflush(stderr) == EOF) {
    return EXIT_FAILURE;
  }

  // raise() demonstrates signal termination explicitly without relying on
  // undefined behavior such as dereferencing a null pointer.
  if (raise(SIGSEGV) != 0) {
    return EXIT_FAILURE;
  }
  return EXIT_FAILURE;
}
