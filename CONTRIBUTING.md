# Contributing to RETRACE

Thank you for helping build RETRACE. The project is currently pre-release and
welcomes focused contributions. Unless stated otherwise, contributions are
submitted under the project's [MIT License](LICENSE).

## Safe Git workflow

Never commit or push directly to `main`. Start from an up-to-date local `main`
and create a focused branch:

```bash
git switch main
git pull --ff-only
git switch -c feat/short-description
```

Install the repository's local safety hooks once per clone:

```bash
git config core.hooksPath .githooks
```

The pre-push hook rejects a destination branch named `main` or `master`, runs a
secret-shape scan, and checks whitespace. Hooks are a convenience, not a
security boundary; maintainers must also enable protected branches on the host.

Before committing, always inspect:

```bash
git status --short
git diff
git diff --cached
scripts/check-secrets.sh
```

Do not commit `.env` files, credentials, private keys, production traces, crash
dumps, customer output, or identifying machine paths. Use synthetic fixtures.

## Build and test

Requirements are Linux, CMake 3.25+, Ninja, and GCC or Clang with C17/C++20
support.

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Or use the convenience script:

```bash
scripts/test.sh
```

Build with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

Build products live below `build/` and are ignored by Git.

## Formatting and warnings

Run formatting after installing `clang-format`:

```bash
scripts/format.sh
```

CI treats supported compiler warnings as errors. Local development leaves
warnings visible without turning every warning into a blocked edit:

```bash
cmake --preset dev -DRETRACE_WARNINGS_AS_ERRORS=ON
```

Intentional warnings should be fixed or narrowly documented—not hidden with a
global flag.

## Tests

The current suite uses small, dependency-free C++ test executables registered
with CTest. It covers CLI behavior, file-descriptor ownership, process launch
and exit handling, and execution of a compiled C stream fixture. As the suite
grows, a test framework may be introduced deliberately.

Planned unit tests will cover parsing, encoding, matching, and rendering.
Planned golden tests will protect trace compatibility and output once the trace
format exists. Sanitizers and static analysis complement the current functional
tests.

Every behavior change should include a test at the lowest useful layer. Error
paths matter as much as successful paths in a tracing tool.

## Pull requests

Keep pull requests narrow and explain the user-visible outcome, tests run,
security/privacy impact, and documentation changes. Do not overstate support:
for example, libc interposition is not complete syscall visibility and scenario
reruns are not deterministic replay.

Maintainers should require pull requests, passing CI, resolved review, and
non-stale approval before merging to protected `main`.
