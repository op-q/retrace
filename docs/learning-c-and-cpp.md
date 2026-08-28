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

The root CMake project enables both `C17` and `C++20`. Small C fixtures under
`tests/` and recorder demonstrations under `examples/` exercise the C toolchain.
The injected C runtime now implements its first bounded handshake slices after
completion of the process-recorder implementation.

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

## Lesson 7: `fork`, `execvpe`, and `waitpid`

The first `run` implementation combines three Linux C APIs:

```text
RETRACE parent
   │
   ├── fork() ──> child calls execvpe() ──> target program
   │
   └── waitpid() <──────────────────────── target completion
```

`fork()` returns twice: once in the parent with the child's process ID, and once
in the child with zero. `execvpe()` does not create another process; on success,
it replaces the child program and never returns. `waitpid()` lets the parent
collect the child's final status instead of leaving a zombie process.

The `p` in `execvpe` asks libc to search `PATH`, which is why both `/bin/echo`
and `python3` work as targets. The `e` supplies an explicit environment array.
RETRACE copies that environment before `fork()`, replaces its private runtime
descriptor setting, and builds both mutable, NUL-terminated `char*` arrays in
the parent. This avoids allocation-heavy environment mutation between `fork()`
and `exec`, where the child should perform only a small set of predictable
operations.

One close-on-exec pipe reports launch errors. A successful `execvpe()` closes the
pipe automatically; a failed call writes its saved `errno`. This distinguishes
“the target returned 127” from “the target could not be launched.”

## Lesson 8: C streams and a test fixture

`tests/fixtures/stream_fixture.c` is the first compiled C source in the project.
It writes deterministic markers and larger test streams through C's standard
I/O API:

```c
fputs("fixture: stdout\n", stdout);
fflush(stdout);
```

`stdout` and `stderr` are separate `FILE*` streams backed by different file
descriptors. Each call returns a status that C code must check explicitly.
`EXIT_SUCCESS` and `EXIT_FAILURE` communicate the final outcome to the parent
process.

`fflush()` matters because a C library may buffer output, especially when a
stream points to a pipe instead of an interactive terminal. RETRACE redirects
both descriptors with `dup2()` and uses `poll()` to read whichever pipe becomes
ready. It rotates which pipe is considered first between polling cycles so a
busy stream cannot starve the other. A fixed-size buffer sends each chunk to a
callback immediately, keeping supervisor memory bounded and leaving a natural
hook for the trace writer.

Even with concurrent reads, a collector must not claim perfect ordering between
two independent pipes. If both are ready before the parent runs, the kernel does
not preserve a single cross-pipe write order for it to recover. Future trace
events can timestamp observed chunks, but those timestamps describe collection
order rather than exact instruction order in the target.

The collector also checks `waitpid(..., WNOHANG)` while polling. After the direct
child exits, a grandchild may still own copies of the stream write ends. Linux
`FIONREAD` lets RETRACE snapshot the bytes already queued; it drains exactly that
bounded amount and closes the read ends instead of waiting forever for unrelated
descriptor owners.

In C, `int main(void)` explicitly declares that a program accepts no arguments.
The expanded fixture instead uses `int main(int argc, char* argv[])` so a test
mode can be selected. Both forms are preferable to an empty parameter list,
whose historical C meaning differs from C++.

## Lesson 9: framing bytes for a durable trace

The trace writer does not write an in-memory C++ struct directly. Struct padding,
native byte order, and type sizes can differ between compilers and machines.
Instead, small encoding functions place each unsigned integer into a byte array
in little-endian order. Strings are stored as a 32-bit byte count followed by
exactly that many bytes, so embedded NUL bytes do not terminate a field.

Each event starts with its total framed length. If RETRACE stops midway through
the final write, a reader can scan the earlier complete lengths and stop at the
incomplete tail. This is crash-tolerant framing, not a guarantee against storage
loss during a machine or filesystem failure.

The move-only `trace::Writer` is another RAII owner: its destructor closes the
trace descriptor. Creation uses Linux flags that reject existing paths and avoid
following a final symlink, and requests mode `0600` so access is limited to the
creating user. A restrictive umask can remove more permissions. The trace
descriptor is close-on-exec so the target cannot accidentally inherit it.

## Lesson 10: bounded parsing and explicit outcomes

A file parser must distrust lengths stored in the file itself. RETRACE's reader
first reads the fixed 16-byte prefix, rejects unsupported versions and headers
larger than 1 MiB, and only then allocates the header payload. A small cursor
checks its remaining byte count before decoding every integer or byte string.
The same pattern applies to each event: validate its declared frame size before
allocating, then verify the redundant payload size, flags, timestamp order, and
known payload shape.

This is also an ownership lesson. `trace::Header` and `trace::Event` contain
owned `std::string` and `std::vector` values rather than views into a reusable
read buffer. A caller can safely retain a decoded value in separate storage
after the next read. The move-only `trace::Reader` owns its descriptor through
the same RAII pattern as the writer.

Parsing has more than two outcomes. `Reader::next()` distinguishes a decoded
event, clean end of input, an incomplete final frame, and an error. Format errors
use a custom `std::error_category`, while operating-system failures retain their
system category. This lets the CLI map malformed, unsupported, and truncated
traces to code `4` without mislabeling a missing file as bad trace data.

Unknown event identifiers illustrate forward-compatible framing: when their
lengths, flags, and timestamp are valid, the reader preserves their payload as
opaque bytes instead of guessing its meaning. The inspector escapes and caps
those bytes before printing them. Finally, clean EOF means only that the parsed
bytes end on a frame boundary. With no v1.0 footer or checksum, it cannot prove
that the recording was finalized or authenticated.

## Lesson 11: process groups, signals, and `chdir`

A process group gives one identifier to a related set of processes. The target
child calls `setpgid(0, 0)` before `execvpe()`, making its PID the group ID.
RETRACE can then send a signal to the whole group with a negative identifier:

```c
kill(-target_process_group, SIGTERM);
```

This reaches descendants that remain in the group, unlike `kill(target_pid,
SIGTERM)`, which addresses only the leader. RETRACE blocks `SIGINT` and `SIGTERM`
around `fork()` and receives them through Linux `signalfd`. Because that
descriptor participates in `poll()` beside stdout and stderr, ordinary C++ code
can forward and record signals without running containers, streams, or callbacks
inside an asynchronous signal handler. The child restores the caller's original
signal mask before replacing its process image, and the parent restores it when
supervision ends.

The selected working directory follows a similar pre-`exec` rule. The CLI first
resolves and validates the directory so a bad path cannot create a misleading
trace. The child still calls `chdir()` and reports its saved `errno` through the
launch-status pipe because the filesystem may change between validation and use.
This repeated check handles the time-of-check/time-of-use boundary honestly.

The C examples show two explicit outcome paths. `normal.c` checks each stream
operation and returns `EXIT_SUCCESS`; `crash.c` calls `raise(SIGSEGV)` so the
recorder observes deliberate signal termination without relying on undefined
behavior.

## Lesson 12: a small C runtime handshake

The first injected-runtime slice is a C17 shared library. A function marked with
the compiler's `constructor` attribute runs when the dynamic loader loads that
library, before the target reaches `main`. Constructor code deserves unusual
restraint: the target has not initialized its application state, and a failure
must not prevent an otherwise valid program from starting.

The runtime therefore performs one bounded action. It parses an explicitly
inherited descriptor without allocation, encodes a fixed 16-byte handshake, and
sends it through a Unix-domain `SOCK_SEQPACKET` socket. The bytes are written
field by field in little-endian order. Copying an in-memory C struct would also
copy compiler-selected padding and native byte order, making the protocol depend
on the build rather than its documented contract.

`MSG_NOSIGNAL` prevents a vanished receiver from delivering `SIGPIPE` to the
target. `MSG_DONTWAIT` prevents a full channel from blocking target startup.
Every failure is ignored after restoring the `errno` value that existed on
entry. This is deliberate fail-open instrumentation: losing observability is
reported by the supervisor, but it does not become a new target failure.

## Lesson 13: sequenced packets and descriptor inheritance

A Unix-domain `SOCK_SEQPACKET` pair is local IPC with message boundaries. Unlike
a byte-stream pipe, one runtime `send()` corresponds to one supervisor
`recvmsg()`. That makes it possible to reject a short, oversized, or
size-mismatched frame without guessing where the next message begins.

Both endpoints start nonblocking and close-on-exec. RAII owns them before
`fork()`. The child closes the supervisor endpoint and clears close-on-exec only
on the target endpoint; the parent does the inverse. The rebuilt environment
names that one inherited descriptor. This is capability-like design: possession
of the descriptor, rather than a global pathname or listening service, grants
access to the session channel.

The supervisor polls the channel beside stdout, stderr, and signals. It accepts
one compatible handshake, emits typed evidence, and rejects malformed or
duplicate packets. Once the direct target exits, it drains only immediately
available messages and closes the channel so a descendant cannot keep
supervision alive indefinitely.

## Lesson 14: loading a guest library deliberately

`LD_PRELOAD` asks the Linux dynamic loader to place a shared library before the
target's ordinary dependencies. RETRACE resolves its own executable through
`/proc/self/exe`, selects the exact build runtime or the installed relative
library directory, and passes an absolute canonical path. The environment is
rebuilt before `fork()` so the child can stay allocation-free: RETRACE's path is
prepended, caller preload entries remain afterward, and the owned channel entry
is replaced rather than trusted.

The descriptor needs opposite close-on-exec states at two moments. The
supervisor first clears `FD_CLOEXEC` so the runtime can receive it during the
target's initial `execvpe()`. The runtime constructor restores `FD_CLOEXEC`
after loading. Hooks can continue using the open descriptor in that process
image, while a later exec cannot accidentally produce a second session
handshake through an inherited endpoint.

Static and secure-execution targets may ignore `LD_PRELOAD`. That is not an
`execvpe()` failure: the target can run successfully without instrumentation.
The supervisor therefore observes and records the final target result, then
classifies the missing required handshake separately. `--no-runtime` makes the
recorder-only choice explicit.

Sanitizers add another loader-order constraint. A sanitizer-instrumented preload
cannot safely enter an arbitrary uninstrumented program because its sanitizer
runtime may be initialized too late. RETRACE keeps the production preload
ordinary and builds a second instrumented copy only for sanitizer-enabled C
tests.

## Habits to practice now

- Read compiler warnings; do not merely silence them.
- State who owns every resource and how long every view remains valid.
- Check the documented success/error contract of each system call.
- Keep C ABI headers free of C++-only types.
- Prefer small translation units with narrow public interfaces.
- Add a failing test before fixing a bug.
- Use sanitizers in addition to tests; they find different classes of mistakes.
