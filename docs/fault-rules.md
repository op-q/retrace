# Fault rules

Fault injection begins after the process recorder and injected runtime are
stable. Rules must be explicit, observable, bounded, reproducible, inspectable,
and safe by default.

## Scenario shape

TOML is the planned scenario format:

```toml
version = 1
seed = 42

[[rules]]
operation = "connect"
match = "127.0.0.1:5432"
action = "delay"
duration_ms = 500
max_activations = 1

[[rules]]
operation = "open"
match = "/tmp/cache/*"
action = "fail"
errno = "EACCES"
after = 2
max_activations = 1
```

A rule may define an operation, match expression, action, count threshold,
probability, delay, error number, signal, and activation limit. Unbounded or
ambiguous behavior should be rejected or require an explicit opt-in.

## Planned actions

Fail an operation:

```toml
operation = "open"
match = "/tmp/cache/*"
action = "fail"
errno = "EACCES"
max_activations = 1
```

Delay a connection:

```toml
operation = "connect"
match = "127.0.0.1:5432"
action = "delay"
duration_ms = 500
max_activations = 1
```

Fail after a number of matches:

```toml
operation = "write"
match = "*"
action = "fail"
errno = "EIO"
after = 3
max_activations = 1
```

Later actions may include short writes and scheduled signals:

```toml
operation = "write"
match = "*"
action = "short"
max_bytes = 8
max_activations = 1
```

```toml
operation = "process"
action = "signal"
signal = "SIGTERM"
after_ms = 5000
max_activations = 1
```

## Determinism and evidence

Probabilistic rules require an explicit or generated seed, stored in the trace.
A fixed seed should reproduce rule decisions, while documentation must avoid
promising complete execution replay.

Matching, delaying, failing, signaling, and killing each create trace events.
RETRACE must never claim a fault was injected without evidence. The trace must
also distinguish an observed target failure from one introduced by RETRACE.

## First delivery

The first fault release will support only:

1. failing a matching `open` with a configured `errno`;
2. delaying a matching `connect`; and
3. scheduled target termination.

Additional operations require a concrete use case, tests for the target's normal
and failing behavior, and a documented performance/safety impact.
