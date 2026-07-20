# Learning C and C++ through RETRACE

This guide follows the implementation order. Each milestone introduces language
features because the product needs them, not as isolated syntax exercises.

## Lesson 1: two languages, two jobs

C and C++ compile to native code, but they encourage different designs.

C gives you functions, structs, pointers, explicit allocation, and a stable ABI
used by Linux and libc. It is a good fit for the future injected runtime because
that library runs inside a target process and must stay tiny and predictable.

C++ builds on C's systems capabilities with constructors, destructors,
templates, containers, stronger types, and RAII. It is a good fit for the main
program because RETRACE will own many resources and coordinate complex states.

The root CMake project enables both `C17` and `C++20`. No production C source is
added yet: the roadmap deliberately finishes process recording before injecting
code into a target. A small C fixture under `tests/` already exercises the C
toolchain and produces deterministic stdout and stderr.

## Lesson 2: compilation and linking

A compiler translates each `.c` or `.cpp` source file into an object file. A
linker combines object files and libraries into an executable or shared library.
Headers are pasted into a translation unit by the preprocessor; they are not
independently linked.

In this scaffold:

```text
src/cli/commands.cpp ─────┐
                         ├─> libretrace_core.a ─┐
src/process/process.cpp ──┘                     │
src/main.cpp ───────────────────────────────────┴─> retrace
```

`retrace_core` keeps CLI behavior testable without launching the final program.
`retrace` contains only the operating-system entry point, `main`.

Try:

```bash
cmake --preset dev
cmake --build --preset dev --verbose
```

The verbose build shows the compile and link commands CMake generated.

## Lesson 3: `main`, arguments, and views

The operating system enters a C or C++ program through:

```cpp
int main(int argc, char* argv[]);
```

`argc` is the number of argument pointers and `argv` points to NUL-terminated
byte strings. `src/main.cpp` immediately converts those raw inputs into
`std::string_view` values. A string view does not own or copy characters; it is
only a pointer plus a length. That is safe here because `argv` remains alive for
the entire call to the CLI.

This is an early ownership lesson: a view is cheap, but the viewed data must
outlive it.

## Lesson 4: namespaces and small interfaces

The CLI declaration lives under `namespace retrace::cli`. Namespaces prevent
collisions; they do not create runtime objects. The public header exposes only
the function and exit codes needed by `main` and tests. Implementation helpers
stay in an unnamed namespace inside the `.cpp` file, which gives them internal
linkage.

This separation reduces rebuilds and prevents accidental dependencies on
private details.

## Lesson 5: tests without a framework

The initial unit test is a normal C++ executable registered with CTest. It calls
the same CLI function as `main`, substitutes string streams for terminal output,
and checks return codes and text.

This shows an important design habit: pass dependencies at a boundary. A
function receiving `std::ostream&` is easy to test; a function that writes
directly to a global terminal is harder.

## Lesson 6: RAII and file descriptors

Linux represents open files and pipes with integer file descriptors:

```c
int fd = open(path, O_RDONLY);
if (fd < 0) {
    /* inspect errno */
}
/* ... */
close(fd);
```

In C, every successful acquisition needs a matching cleanup on every control
flow path. RETRACE's move-only `UniqueFd` wrapper calls `close()` in its
destructor. Early returns therefore cannot accidentally leak the descriptor.

Copying is deleted because two owners would both try to close the same integer.
Moving transfers the descriptor and changes the previous owner to `-1`. This is
the difference between an owning handle and a borrowed integer.

## Lesson 7: `fork`, `execvp`, and `waitpid`

The first `run` implementation combines three Linux C APIs:

```text
RETRACE parent
   │
   ├── fork() ──> child calls execvp() ──> target program
   │
   └── waitpid() <──────────────────────── target completion
```

`fork()` returns twice: once in the parent with the child's process ID, and once
in the child with zero. `execvp()` does not create another process; on success,
it replaces the child program and never returns. `waitpid()` lets the parent
collect the child's final status instead of leaving a zombie process.

The `p` in `execvp` asks libc to search `PATH`, which is why both `/bin/echo` and
`python3` work as targets. The argument list must be a mutable, NUL-terminated
array of `char*`, ending with a null pointer. RETRACE copies its non-owning
`string_view` inputs into owned `std::string` storage before building that C ABI
array.

One close-on-exec pipe reports launch errors. A successful `execvp()` closes the
pipe automatically; a failed call writes its saved `errno`. This distinguishes
“the target returned 127” from “the target could not be launched.”

## Lesson 8: C streams and a test fixture

`tests/fixtures/stream_fixture.c` is the first compiled C source in the project.
It writes deterministic markers through C's standard I/O API:

```c
fputs("fixture: stdout\n", stdout);
fflush(stdout);
```

`stdout` and `stderr` are separate `FILE*` streams backed by different file
descriptors. Each call returns a status that C code must check explicitly.
`EXIT_SUCCESS` and `EXIT_FAILURE` communicate the final outcome to the parent
process.

`fflush()` matters because a C library may buffer output, especially when a
stream points to a pipe instead of an interactive terminal. Even with explicit
flushes, a collector reading two different descriptors must not assume their
display order perfectly reconstructs execution order. RETRACE will read both
concurrently and timestamp chunks when stream capture is implemented.

In C, `int main(void)` explicitly declares that the program accepts no
arguments. This is preferable to an empty parameter list, whose historical C
meaning differs from C++.

## Habits to practice now

- Read compiler warnings; do not merely silence them.
- State who owns every resource and how long every view remains valid.
- Check the documented success/error contract of each system call.
- Keep C ABI headers free of C++-only types.
- Prefer small translation units with narrow public interfaces.
- Add a failing test before fixing a bug.
- Use sanitizers in addition to tests; they find different classes of mistakes.
