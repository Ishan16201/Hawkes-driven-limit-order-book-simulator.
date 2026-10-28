# Event-driven simulation

Canonical events use integer nanoseconds and are sorted by `(timestamp_ns,
sequence)`. `sequence` must be globally unique in a source, so equal timestamps
have an explicit deterministic order. The simulation clock advances only from
these event timestamps; it never observes wall-clock time.

`DeterministicEventSource` supports hand-written tests. `HistoricalReplaySource`
is its normalized-event alias until a historical input adapter exists.
`generate_seeded_limit_events` supplies a basic deterministic synthetic source;
it is not a Hawkes model. Runs record schema version, source identity,
configuration ID, software revision, and optional seed. Reproducibility also
requires preserving the exact normalized input and configuration.

Optional dispatch logs are NDJSON with `schema_version` on each record. Summary
statistics are returned separately and are not mixed into that event log.
