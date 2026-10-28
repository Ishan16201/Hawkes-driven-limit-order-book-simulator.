# Architecture and implementation status

## Design boundary

The exchange is deterministic and does not depend on stochastic generation,
historical data, estimation, analytics, or strategy policy. Upstream components
produce typed commands; the exchange applies them in invocation order. The
architecture is implemented in one CMake library (`lob_core`) with separate
headers/namespaces rather than separate link libraries.

```text
core types
  ├── exchange order book + matching engine
  │     ├── simulator (canonical event source / deterministic replay)
  │     ├── Hawkes market adapter (stochastic events + marks)
  │     └── strategy broker/evaluator
  ├── Hawkes process + parameter validation
  ├── historical parser / event mapping
  ├── estimator
  └── analytics
```

The arrow diagram is a package-level summary, not permission for the exchange
to call any generator. In particular, `MatchingEngine` accepts complete limit,
market, and cancellation commands and uses no RNG.

## Components

| Component | Main implementation | Responsibility |
|---|---|---|
| Core | `include/lob/core/` | Typed integer IDs, ticks, lots, timestamps, sides |
| Exchange | `src/exchange/` | Price levels, FIFO queues, order index, matching, trades, cancellations |
| Replay simulator | `src/simulation/` | Canonical event ordering, deterministic dispatch, run summary, replay log |
| Hawkes model | `src/stochastic/hawkes_process.cpp` | Exponential state, exact decay, Ogata thinning, seedable RNG |
| Market adapter | `src/stochastic/hawkes_market_adapter.cpp` | Hawkes event-to-order marks, initial book, snapshots, event/fill outputs |
| Historical adapter | `src/historical/event_parser.cpp` | Basic CSV/NDJSON parsing, taxonomy mapping, conversion to replay commands |
| Estimation | `src/estimation/hawkes_estimator.cpp` | Point-process likelihood, gradients, constrained profile fitting, diagnostics |
| Analytics | `src/analytics/` | Descriptive microstructure statistics over simulation results |
| Strategy | `src/strategy/` | Causal strategy context, broker, event-driven accounting, baseline policies and evaluation |

`src/main.cpp` is a fixed-configuration synthetic example. It does not parse the
JSON reference configuration. Optional output flags write complete event,
trade, manifest, and analytics files.

## Causality and deterministic replay

The strategy API receives current-time snapshots, trades, execution reports,
portfolio state, and open orders. The evaluator owns the future event queue and
does not pass it to strategy code. See [strategy.md](strategy.md) for action,
latency, mark-to-market, and risk details.

`DeterministicEventSource` validates unique event IDs/sequences and the
`Simulator` orders by `(timestamp_ns, sequence)`. Historical replay currently
uses the same event-source type after parsing/mapping. It does not reproduce
venue queue priority or hidden exchange state absent from the source records.

The Hawkes market adapter separately runs the process and then maps each
pre-event type into an exchange command. Its random stream is seeded and its
parameters/marks are included in output manifests.

## Ownership and memory

The exchange retains accepted order IDs for the lifetime of a matching-engine
run, and retains all exchange events and trades. The replay simulator can
disable its additional dispatch vector, but this does not disable engine
history. The Hawkes adapter returns every generated event, every trade, and a
book snapshot at every event boundary; memory is therefore unbounded in run
length and snapshots can grow with book depth. NDJSON serialization creates an
additional in-memory string before writing. There is no streaming or
bounded-history mode yet.

## Build graph and dependencies

The project is C++20. `lob_core` has no runtime network or Python dependency.
Catch2 v3.8.0 is pinned to commit
`914aeecfe23b1e16af6ea675a4fb5dbd5a5b8d0a` and fetched only when configuring
tests. CMake targets are `lob_core`, `lob_simulator`, `lob_tests`, and
`lob_benchmarks`.

The accepted target architecture is described in [design.md](design.md). That
document is a system specification; implemented and unimplemented scope is
tracked in [validation.md](validation.md) and [research_report.md](research_report.md).
