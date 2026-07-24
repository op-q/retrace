# Security and privacy model

RETRACE currently executes another program, relays its stdout and stderr through
bounded in-memory chunks, can persist selected run data to an explicit trace
path, and can inspect or structurally validate v1.0 trace files. It does not yet
inject a shared library. Runtime injection handles another sensitive boundary,
so its security properties remain requirements—not claims about the current
build.

## Current behavior

- The current `run` command does not require root.
- RETRACE launches only the command supplied after `run --` and waits for it.
- The target inherits the invoking environment and standard input, except that
  every caller-provided `RETRACE_RUNTIME_EVENT_FD` entry is replaced with the
  supervisor's intended endpoint; stdout and stderr are redirected to RETRACE
  pipes.
- The target runs as the leader of a new process group. `SIGINT` and `SIGTERM`
  received during supervision are forwarded to that group.
- `--working-directory PATH` is resolved and checked before trace creation; the
  child independently changes directory before replacing its process image.
- RETRACE concurrently relays stdout and stderr without storing a complete copy.
- `--output TRACE` records command arguments, working directory, lifecycle, and
  stdout/stderr chunks. It does not record environment variables, file contents,
  or network payloads.
- A trace path requests mode `0600` (a restrictive umask may remove more
  permissions) and uses close-on-exec, exclusive creation, and no final-component
  symlink following. Existing paths are not overwritten, and the target is not
  started if trace creation fails.
- `inspect` and `validate` require a regular file. Their reader enforces the
  1 MiB header and per-event payload limits before allocating, bounds the
  argument count, checks every metadata field and frame, and reads events
  incrementally.
- `inspect` quotes untrusted byte fields, escapes control and non-printable
  bytes, and caps displayed metadata, arguments, and event-payload previews.

## Required defaults for capture features

- Only the launched command and intended descendants are in scope.
- Environment variables, file contents, and network payloads are not recorded
  by default.
- Injected behavior is always labeled in the trace.
- Readers treat trace files and event channels as untrusted input.

Paths, command arguments, stdout, and stderr can still contain secrets. Review a
trace before publishing it. Planned controls include argument and path
redaction, explicit environment allowlists, and disabling stream capture.

## Parser and runtime requirements

The writer caps header and event payloads and uses explicit byte encodings. The
reader validates lengths before allocating, uses checked cursor arithmetic,
never assumes NUL termination, rejects malformed known-event payloads, and
accepts otherwise valid unknown event identifiers as opaque bounded bytes.
Malformed input fails cleanly, while an incomplete final frame is reported
separately so preceding complete frames can still be inspected.

These checks provide structural parsing, not trust or authenticity. Version 1.0
has no footer, checksum, signature, or finalization record. A clean frame-boundary
EOF therefore cannot prove that the original writer finished, that a suffix was
not lost, that bytes were not modified, or that data is safe to disclose.

The runtime handshake preserves `errno`, avoids allocation, and lets the target
continue if its event channel fails. The supervisor bounds and validates each
sequenced packet and rejects malformed protocol input. Before operation
interposition ships, hooks must additionally prevent recursion and minimize
allocation and locking. Runtime instrumentation will not offer container-grade
isolation.

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
