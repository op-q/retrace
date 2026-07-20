# Security policy

## Supported versions

RETRACE has not released a supported version yet. Security reports about the
current development branch are still welcome.

## Reporting a vulnerability

Please use this repository's **Security → Advisories → Report a vulnerability**
flow. If that button is unavailable, open a minimal public issue asking the
maintainers to enable private reporting or provide a private contact. Do not put
vulnerability details in that issue.

Do not open a public issue for an unpatched vulnerability and do not include
real credentials, private traces, customer data, or unnecessary exploit data.
Use synthetic reproduction inputs wherever possible.

A useful report includes the affected revision, platform/compiler, impact,
minimal reproduction, and any suggested mitigation. Maintainers should
acknowledge a report privately, agree on disclosure timing, and credit the
reporter if requested.

## Sensitive trace data

Trace files can include command arguments, paths, standard output, and standard
error. Treat them as sensitive until reviewed. RETRACE does not yet provide a
guarantee that a trace is safe to publish.
