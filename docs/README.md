# Documentation map

This directory is the engineering and product reference for RETRACE. The root
[README](../README.md) is the quick entry point; the documents here define the
boundaries, behavior, formats, and release criteria in more detail.

## Start here

Read these in order when learning the project:

1. [Product goals and scope](product.md) — the problem, intended users, goals,
   and explicit non-goals.
2. [Architecture](architecture.md) — the implemented component boundaries and
   the planned C runtime.
3. [CLI](cli.md) — current commands, exit behavior, and clearly labeled planned
   options.
4. [Recorder examples](examples.md) — small normal and intentional-crash target
   programs.
5. [Development design](development.md) — repository layout, language choices,
   error model, and testing policy.
6. [Roadmap](roadmap.md) — milestone order and acceptance criteria.

## Reference documents

| Document | Purpose |
| --- | --- |
| [Trace format](trace-format.md) | The versioned on-disk contract, event schemas, bounds, and reader behavior |
| [Runtime protocol](runtime-protocol.md) | The live C-runtime handshake and event-channel framing contract |
| [Security model](security.md) | Process, filesystem, parser, trace-data, and repository safety boundaries |
| [Fault rules](fault-rules.md) | Planned scenario language and fault evidence requirements |
| [C and C++ learning guide](learning-c-and-cpp.md) | Explanations of the native concepts used by the implementation |
| [v0.1 release checklist](release-checklist.md) | Required automated, manual, documentation, and safety evidence before release |

Repository-wide collaboration and reporting documents remain at the root:

- [Contributing](../CONTRIBUTING.md)
- [Security policy](../SECURITY.md)
- [Code of conduct](../CODE_OF_CONDUCT.md)

## Where information belongs

- User-visible commands and exit behavior belong in `cli.md`.
- Stable process, component, and C/C++ boundaries belong in `architecture.md`.
- Binary compatibility details belong in `trace-format.md`.
- Capture, parsing, privilege, and sensitive-data rules belong in `security.md`.
- Implemented milestones and future sequence belong in `roadmap.md`.
- Repeatable release evidence belongs in `release-checklist.md`.

Avoid copying the same detailed contract into several documents. Prefer a short
summary and a link to its source of truth.

## Documentation rules

- Clearly distinguish implemented, planned, deferred, and unsupported behavior.
- Update behavior documentation and tests in the same change.
- Do not claim deterministic replay, full syscall visibility, complete process
  trees, durable trace finalization, or container-grade isolation.
- Use synthetic commands and fixtures. Never add private traces, crash dumps,
  customer output, credentials, or identifying production paths.
- Keep the C17/C++20 boundary aligned with `architecture.md`.
- Add a focused document only when it owns a durable contract or repeatable
  workflow; do not create folders for speculative future components.
