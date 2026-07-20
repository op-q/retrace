# Security and privacy model

RETRACE currently executes another program and returns its status. It does not
yet create traces, capture streams, parse trace files, or inject a shared
library. Those planned features handle sensitive and untrusted data, so their
security properties are requirements—not claims about the current build.

## Current behavior

- The current `run` command does not require root.
- RETRACE launches only the command supplied after `run --` and waits for it.
- The target currently inherits the invoking terminal and environment.
- RETRACE does not currently record environment variables, file contents,
  network payloads, stdout, or stderr.

## Required defaults for capture features

- Only the launched command and intended descendants are in scope.
- Environment variables, file contents, and network payloads are not recorded
  by default.
- Injected behavior is always labeled in the trace.
- Trace files are created with user-only permissions by default.
- Output paths are validated; unsafe symlink and overwrite behavior is avoided.
- Readers treat trace files and event channels as untrusted input.

Paths, command arguments, stdout, and stderr can still contain secrets. Review a
trace before publishing it. Planned controls include argument and path
redaction, explicit environment allowlists, and disabling stream capture.

## Parser and runtime requirements

Before trace support ships, trace and event parsers must validate lengths before
allocating, cap payload and string sizes, check integer arithmetic, never assume
NUL termination, and handle unknown or corrupt data without memory-unsafe
behavior.

Before runtime injection ships, the injected runtime must preserve `errno`,
prevent recursion, minimize allocation and locking inside hooks, and normally
let the target continue if its event channel fails. It will not offer
container-grade isolation.

## Repository secret policy

Never commit credentials, tokens, private keys, `.env` files, private traces,
production paths, or copied customer output. Use obviously fake placeholders in
tests and documentation. `.env.example` may contain names and non-sensitive
dummy values only.

The repository includes `scripts/check-secrets.sh` as a fast local safety net.
It blocks common private-key and provider-token shapes and suspicious secret
filenames, but it cannot prove that a commit is safe. Before publishing:

1. inspect `git status` and the complete diff;
2. run `scripts/check-secrets.sh`;
3. confirm generated traces and build artifacts are untracked;
4. check commit history, not only the working tree; and
5. rotate any credential immediately if it was ever committed.

Deleting a secret in a later commit does not remove it from Git history.

## Branch protection

Development must happen on topic branches, never directly on `main`. The local
pre-push hook rejects pushes whose destination is `main` or `master`, but local
hooks can be bypassed. The hosting service must also protect `main` by requiring
pull requests and passing checks, rejecting force pushes and deletions, and
restricting or removing bypass permissions.

## Publishing checklist

- A deliberate license has been selected and committed.
- The security policy names a private reporting path.
- Branch protection and secret scanning are enabled on the host.
- CI uses least-privilege permissions.
- No trace fixture contains data captured from a real private workload.
- Dependencies and CI actions have been reviewed and are kept current.

See [SECURITY.md](../SECURITY.md) for vulnerability reporting.
