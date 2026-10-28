# Hawkes-Driven Limit Order Book Simulator — Design

## 1. Scope and architecture

This is the accepted system specification and target architecture. The
repository now contains a partial implementation; it is not evidence that every
capability below has shipped. See [architecture.md](architecture.md),
[validation.md](validation.md), and [research_report.md](research_report.md) for
the current implementation boundary and verified results. This document's
mathematical definitions remain normative unless a documented model change is
approved.

The system has six independent responsibilities:

1. Exchange mechanics: deterministic price-time-priority matching.
2. Stochastic event generation: seedable synthetic order flow.
3. Historical ingestion/replay: normalization into canonical events.
4. Statistical estimation: Hawkes and mark calibration outside the exchange.
5. Microstructure analytics: book, trade, lifetime, clustering, return, and
   impact measurements.
6. Strategy execution/evaluation: causal orders, fills, costs, and PnL.

Dependency direction:

```text
core/types -> exchange -> event log -> analytics
     |           ^             ^          ^
     +-> Hawkes -+             |          |
     +-> history/replay -------+    strategy/broker
     +-> estimation ----------------------+
```

The exchange depends on typed domain objects and an event-source interface, but
never on Hawkes classes, estimators, file formats, or strategy code. Synthetic
events, deterministic hand-written events, and historical replay therefore use
the same exchange.

## 2. Target repository layout

The following is the intended target layout, not a claim about the current
source tree:

```text
include/lob/{core,exchange,events,stochastic,historical,estimation,analytics,strategy,experiment}
src/                # implementation mirroring include/lob
tests/              # unit, property, statistical, replay, regression tests
benchmarks/         # reproducible microbenchmarks
examples/           # tiny deterministic scenarios
configs/            # versioned model and experiment configurations
docs/               # design, data contract, methodology, validation reports
```

The current CMake build uses a combined `lob_core` library and provides
`lob_simulator`, `lob_tests`, and `lob_benchmarks`. Splitting the core into
separate component libraries and adding a general experiment CLI remain
aspirational.

## 3. Core data contracts and units

The canonical time unit is integer nanoseconds. Historical timestamps are epoch
nanoseconds; synthetic timestamps are nanoseconds from simulation origin. Use a
signed 64-bit `TimestampNs` and `DurationNs`. Convert to `double` seconds only
at explicit statistical boundaries.

- `PriceTicks`: signed 64-bit integer ticks; tick size is metadata, never part of
  matching arithmetic.
- `QuantityLots`: unsigned 64-bit integer lots/shares; lot size is metadata.
- `OrderId`, `TradeId`, `EventId`: unsigned 64-bit identifiers, unique per run.
- `Side`: `Bid` or `Ask`.
- `OrderType`: `Limit` or `Market`; cancel/replace are commands.
- `TimeInForce`: initially `GoodForDay`, `ImmediateOrCancel`, and `PostOnly`
  where applicable.

Canonical commands:

```text
AddLimit(t, id, side, price_ticks, quantity_lots, tif)
Cancel(t, order_id, quantity_to_cancel)
Replace(t, order_id, new_price?, new_quantity)
SubmitMarket(t, id, side, quantity_lots, tif)
```

Canonical outputs include accepted/rejected commands, book changes, trades,
fills, cancellations, and snapshots. Matching is synchronous: all consequences
of a command complete before the next command. Same-timestamp ordering is an
explicit input sequence, never inferred from wall-clock values.

## 4. Matching-engine rules

The baseline exchange is one continuous central limit order book:

- bids match the lowest ask when prices cross; asks match the highest bid;
- each price level is FIFO by accepted sequence number;
- a marketable limit order consumes multiple price levels until filled or no
  longer marketable;
- trades execute at the resting order price;
- market residuals expire/reject under configured TIF and never silently rest;
- a non-marketable limit order rests at its limit price;
- cancels are idempotently rejected after completion/cancellation and support
  partial quantity cancellation;
- replace is cancel-and-new, except an explicitly configured quantity reduction
  may retain priority;
- no hidden liquidity, auction, latency, fees, self-match prevention, or engine
  randomness is in the baseline.

Invariants after every command: no negative quantity, no order both live and
terminal, FIFO preservation, no crossed book, conservation through fills/cancels,
and trade prices equal resting prices.

## 5. Event types and hypotheses

The initial six-dimensional flow is:

```text
LB  limit addition on bid
LA  limit addition on ask
CB  cancellation on bid
CA  cancellation on ask
MB  aggressive buy / marketable flow
MS  aggressive sell / marketable flow
```

These are hypotheses to test, not asserted market facts. Candidate relationships
include same-side and opposite-side limit clustering, cancellation clustering
after additions, aggressive-flow self/cross excitation, and replenishment or
cancellation responses after aggressive trades. The estimated matrix, controls,
and out-of-sample diagnostics decide whether those relationships are useful.

Flow events are not complete exchange commands. A separate mark model chooses
price offset, quantity, TIF, and cancellation target. A cancel can target only a
currently live, previously observed order under a documented selection policy.

## 6. Hawkes specification

For type `i`:

```text
lambda_i(t) = mu_i + sum_j integral_0^t phi_ij(t-s) dN_j(s)
phi_ij(u) = alpha_ij * beta_ij * exp(-beta_ij*u), u >= 0
```

Units and constraints:

- time and decay argument: seconds at the statistical boundary;
- `lambda_i`, `mu_i`: events/second;
- `alpha_ij`: dimensionless expected direct children of type `i` from one type
  `j` event;
- `beta_ij`: inverse seconds, strictly positive;
- `phi_ij`: events/second per event; its integral is `alpha_ij`;
- `N_j`: counting process with dimensionless increments.

Let `A = [alpha_ij]`, with rows as child types and columns as parent types. For
nonnegative kernels, stationary stability requires

```text
rho(A) < 1
```

where `rho` is the spectral radius of the **integrated kernel/branching matrix**.
For finite nonnegative `mu`, the stationary mean exists and is finite under this
condition:

```text
bar_lambda = (I - A)^(-1) mu.
```

Testing `rho(alpha * beta)` or another unintegrated amplitude matrix is invalid
without unit normalization. Holding `alpha` fixed, `beta` changes temporal shape
but not branching mass. Negative baselines, nonpositive decay, nonfinite values,
and `rho(A) >= 1` are rejected for the stationary baseline model.

The generator uses Ogata thinning with exact exponential state updates. Maintain

```text
g_i(t) = sum_j sum_{k:type j} alpha_ij*beta_ij*exp(-beta_ij*(t-t_k))
```

Decay state to each candidate time, sample a waiting time from a valid total
intensity upper bound, accept with `lambda_total / bound`, then choose type
proportional to current intensities. The bound construction must be proven and
tested; invalid bounds fail loudly. A wrapped deterministic `std::mt19937_64`
(or a later benchmark-justified engine) records seed, configuration identity,
proposal count, acceptance count, and terminal state. The generator never mutates
the exchange.

## 7. Historical format and estimation

The MVP canonical interchange is versioned NDJSON, with a strict CSV adapter:

```text
schema_version, instrument, timestamp_ns, source_sequence, event_type, side,
external_order_id, price_ticks, quantity_lots, remaining_quantity_lots,
exchange_order_id, flags
```

A dataset manifest must specify timestamp epoch/timezone, price and lot scales,
source ordering, identifier semantics, missing-field rules, and provenance. Raw
records are never silently coerced; normalization emits errors/warnings and a
report. Replay consumes normalized records through the same `EventSource` API.
Parquet/Arrow is an optional measured extension, not an MVP requirement.

Estimation consumes event histories and emits versioned artifacts containing
event mapping, time units, `mu`, `alpha`, `beta`, constraints, optimizer settings,
data intervals, and diagnostics. Implement exponential-kernel likelihood,
finite-difference/analytic gradient checks, constrained fitting, out-of-sample
likelihood, and residual/count-dispersion diagnostics. Compare Hawkes against
independent Poisson and restricted Hawkes controls. The simulator never fits at
runtime.

## 8. Strategies, costs, and analytics

Strategies observe only current/past events or snapshots and submit through the
same broker interface. The evaluator records decisions, acceptance, queue-ahead
estimate, fills, partial fills, cancellations, inventory, mark-to-market PnL,
fees, spread, slippage, and implementation shortfall. Latency and cost models
are explicit configuration; no future book, trade, or end-of-day information is
available.

Required metrics include spread, midprice, multi-level depth, imbalance, order
lifetime, cancellation rate, queue-conditioned fill probability, event
autocorrelation/cross-correlation, overdispersion, short-horizon returns,
realized/effective spread, signed impact, passive adverse selection, net PnL,
turnover, fill ratio, inventory, drawdown, and confidence intervals.

Experiments progress from exchange correctness, to Hawkes-vs-Poisson clustering,
to mark/book calibration, historical spread/depth/lifetime/return comparison,
queue and passive-fill studies, and aggressive-impact/cost-sensitive strategy
studies.

Every report must be labeled either **synthetic plausibility** (internal
invariants and qualitative behavior) or **empirically validated** (specified
historical data, training/test split, calibration, and statistical acceptance
test). A plausible-looking path is not validation against real data.

## 9. Testing and benchmarking

Use Catch2 v3 consistently, pinned through CMake `FetchContent` at an exact
release/commit with a documented hash. Runtime has no network dependency. CMake
requires C++20, warnings, and Debug/Release plus sanitizer presets where
supported.

Test layers:

- unit tests for units, FIFO, matching, TIF, cancellation, invariants, and RNG;
- property tests for randomized command sequences and quantity conservation;
- mathematical tests for kernel integrals, decay, mean intensity, stability, and
  likelihood/gradient agreement;
- statistical tests for `alpha=0` Poisson reduction and stable mean/count checks;
- integration tests proving equivalent synthetic, hand-written, and historical
  commands yield identical exchange output;
- golden event-log and parameter-artifact regression fixtures.

Benchmark command throughput, per-command latency/allocations, Hawkes proposals
and acceptances, parser/replay throughput, analytics throughput, and peak memory.
Record compiler, CPU, build type, seed, event count, and configuration. No
performance claim is accepted without a reproducible manifest.

## 10. Milestones and acceptance criteria

### M0 — skeleton and contracts

Create CMake targets, warnings, Catch2, configuration/manifest schemas, and this
document. Acceptance: clean configure/build/test on Apple Clang 21/CMake 4.4.2;
seed/configuration manifests serialize; runtime needs no network.

### M1 — deterministic exchange

Implement typed commands, book, matching, fills, cancellation, TIF, and
invariants. Acceptance: golden scenarios cover FIFO, partial/multi-level fills,
non-crossing/crossing orders, cancel races, and market residuals; repeated runs
produce byte-identical logs.

### M2 — sources and replay

Implement `EventSource`, hand-written source, normalized CSV/NDJSON parser,
validation report, and replay. Acceptance: equivalent representations match
exactly; malformed units, timestamps, IDs, and targets fail explicitly.

### M3 — Hawkes generator

Implement six types, exponential state, thinning, seed control, diagnostics, and
mark-to-command adapter. Acceptance: `alpha=0` reduces to independent Poisson;
stable mean/count diagnostics pass; invalid or unstable parameters are rejected;
recorded seeds reproduce every event.

### M4 — estimation and analytics

Implement likelihood, fitting artifacts, diagnostics, and required metrics.
Acceptance: gradient checks pass, train/test separation is enforced, provenance
is present, and metrics recompute from logs.

### M5 — strategies and research harness

Implement causal broker, costs, passive/aggressive controls, experiment manifests,
and summaries. Acceptance: future-data access is impossible through the reviewed
API; fills/costs reconcile; repeated manifests reproduce outputs and hashes.

### M6 — validated extensions

Add richer marks, latency, queue modeling, optional formats, calendars, and
parallel replications only with ablations, benchmark evidence, and an
out-of-sample historical validation report.

## 11. MVP and research-grade extension

MVP: one instrument, one continuous book, integer ticks/lots, six Hawkes event
types, exponential kernels, Ogata thinning, CSV/NDJSON normalization, deterministic
replay, core analytics, and simple passive/aggressive controls.

Research-grade extension: validated venue adapters, latency and queue models,
richer price/size marks, robust estimation and uncertainty, multi-session
calendars, Parquet/Arrow ingestion, independent parallel replications,
experiment registry, and reproducible calibration/validation reports.

## 12. Risks and unresolved empirical decisions

Controls are required for wrong stability normalization, time-unit mismatch,
invalid thinning bounds, numerical under/overflow, near-critical explosion,
queue/mark leakage, look-ahead bias, book bugs, overfitting, plausibility being
mistaken for validation, and non-reproducible artifacts. Use explicit units,
finite checks, invariant tests, causal APIs, time-separated data, controls,
manifests, seeds, and output hashes.

Decisions that require evidence rather than preference are: exact event taxonomy;
whether executions are a separate Hawkes dimension; shared versus type-specific
decays; price/size/cancel-target marks; available historical depth and identifiers;
venue latency, queue, fee, tick, lot, calendar, and self-trade rules; empirical
similarity thresholds; parallel RNG design; and whether Eigen or Arrow/Parquet
provides enough measured benefit to justify its dependency surface.

No data availability, measured performance, or empirical market finding is
claimed by this document.
