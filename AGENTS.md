# Repository working rules

These rules apply to automated coding agents and human-assisted automation in
this repository.

## Git safety

- Never commit or push unless the user explicitly requests it.
- Never push directly to `main` or `master`; use a topic branch and pull request.
- Never force-push, rewrite history, delete branches, or discard user changes
  without explicit approval.
- Treat all pre-existing working-tree changes as user-owned.
- Before any requested publication, inspect the complete diff and run
  `scripts/check-secrets.sh`.

## Private data

- Do not read or expose `.env` files, credentials, private keys, private traces,
  crash dumps, customer data, or unrelated environment variables.
- Do not add real tokens, hostnames, user paths, process output, or captured
  production data to source, tests, docs, logs, commits, issues, or pull requests.
- Use synthetic fixtures and obviously fake placeholders.
- Do not add telemetry, external uploads, or network access without explicit user
  direction and documentation.
- Treat `.rtc` traces as sensitive even when they are generated locally.

## Engineering workflow

- Keep the current milestone narrow; do not begin the injected runtime before
  the process recorder is reliable.
- Build and run focused tests after changes when a toolchain is available.
- Preserve the C17/C++20 boundary described in `docs/architecture.md`.
- Explain relevant C or C++ concepts while implementing them so the repository
  remains useful as a learning project.
- Start every new comment-capable source, header, test, script, build, and
  configuration file with a short summary of the file's overall purpose. Put
  it at the first legal comment position, after a shebang when one is required.
- Add focused comments for ownership, data flow, safety constraints, and other
  non-obvious decisions. Explain why the code exists without narrating trivial
  syntax. Formats that do not support comments, such as standard JSON, are
  exempt.
- Keep support claims aligned with implemented and tested behavior.
