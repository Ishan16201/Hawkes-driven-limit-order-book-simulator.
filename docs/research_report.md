# Research report: Hawkes-driven limit order book simulator

## 1. Research motivation

The project provides a reproducible software environment for studying how
clustered order-flow arrival models interact with deterministic limit-order-book
matching. The simulator separates the exchange from stochastic arrivals so
that event generators, replay sources, strategies, and analytics can share the
same matching implementation.

## 2. Research questions

Questions that the current architecture can support in controlled synthetic
experiments include:

1. How does exponential self/cross-excitation change event counts and
   inter-arrival clustering relative to independent Poisson flow?
2. How do mark assumptions and order-book matching map generated events into
   spreads, depth, fills, and short-horizon price changes?
3. How do strategy execution costs and inventory evolve under those synthetic
   paths?

These are research questions, not established findings. The current model
parameters and mark rules are illustrative.

## 3. Mathematical background

For type `i`, the conditional intensity is baseline plus the history-weighted
sum of type-`j` events. With
`phi_ij(u)=alpha_ij*beta_ij*exp(-beta_ij*u)`, the kernel mass is `alpha_ij`.
For nonnegative kernels, stationarity requires `rho(A)<1`, with `A=alpha`, and
stationary mean `(I-A)^(-1)mu`. This normalization is used by the implementation.
See [mathematical_model.md](mathematical_model.md) for formulas, units, and the
recursive state.

## 4. Exact Hawkes specification

The implementation uses a six-dimensional event vector: limit buy, limit sell,
buy cancel, sell cancel, market buy, market sell. Baselines are positive events
per second; alpha is dimensionless; beta is inverse seconds. Each event adds
`alpha_ij*beta_ij` to child intensity `i` for parent event type `j`. Event
intensities recorded at event times are pre-jump.

## 5. Kernel normalization and stability

The exponential kernel integrates to alpha, so beta affects temporal shape but
not branching mass. The validator uses spectral radius of alpha, not of
`alpha*beta`. The spectral-radius calculation was changed to shifted power
iteration to avoid periodic norm oscillation; tests include a hand-checkable
two-type periodic matrix. This verifies selected cases, not every conditioning
regime.

## 6. Numerical event-generation method

The process uses exact exponential state decay and Ogata thinning with current
total intensity as the proposal upper bound. Accepted types are sampled in
proportion to their pre-event intensities. `std::mt19937_64` is seeded from
configuration; adapter marks use a separate XOR-mixed seed. Hawkes times are
seconds and are floored to integer nanoseconds before exchange processing.

## 7. Exchange matching rules

The exchange uses best-price then FIFO matching, executes at the resting price,
allows partial fills and multi-level sweeps, rests nonmarketable limit residuals,
expires market residuals, and removes whole remaining orders on cancellation.
It has no venue-specific queue, hidden-liquidity, latency, fee, auction, or
self-trade rules. See [exchange_rules.md](exchange_rules.md).

## 8. Order and cancellation generation assumptions

The adapter separates event timing/type from marks. Limit prices are placed
relative to the current best quote or a fallback reference price; sizes are
uniform integer draws over configured lot ranges; market orders are immediate;
default cancellation targets the front order at the best level. The optional
random cancellation policy also samples only the best level. These are explicit
synthetic assumptions, not empirically estimated distributions.

## 9. Data sources and limitations

No actual suitable market dataset is included or used. The repository includes
a small synthetic CSV fixture and generated Hawkes paths only. The CSV/NDJSON
parser is a basic adapter and is not certified for any venue feed. Empirical
spread, depth, lifetime, cancellation, trade-size, return, clustering, and
post-trade impact validation is **incomplete**.

## 10. Parameter estimation procedure

The estimator implements a point-process likelihood, analytic baseline/alpha
gradients, a Poisson comparator, projected gradient ascent, deterministic
restarts, standard-error diagnostics, and compensator residual summaries.
Decay is fixed or selected from a discrete common-decay grid `{1,5,10}`; it is
not optimized continuously. Parent-specific shared decay is explicitly
unsupported. The reported likelihood-ratio tail is approximate and should not
be treated as formal inference. No calibration executable or empirical fitting
has been completed; see [calibration.md](calibration.md).

## 11. Validation design

The completed software checks include hand-calculated matching, randomized
small-book comparison against an independent reference model, deterministic
replay, order-book invariants, quantity overflow, Hawkes recursion/direct-sum,
periodic-matrix stability, empty-history likelihood, and residual diagnostics.
Synthetic checks include fixed-seed path behavior, an across-seed stationary
rate check, and limited five-seed recovery/misspecification diagnostics. These
tests do not establish goodness of fit for real markets.

## 12. Benchmark methodology

Release benchmarks run five repetitions and report medians for best-price
lookup, insertion, cancellation, matching, Hawkes state updates/event-time
simulation, event-log serialization, and deterministic end-to-end simulation.
Environment, inputs, throughput, output size, and limitations are tabulated in
[benchmarking.md](benchmarking.md). No claim is made for other machines or
configurations.

## 13. Results actually obtained

- The documented synthetic executable produced 72 Hawkes events and 10
  exchange fills for its fixed illustrative 10-second configuration. This is a
  reproducibility smoke test, not a market result.
- Release, Debug, and AddressSanitizer/UndefinedBehaviorSanitizer builds each
  passed all 85 tests in the final local checks; exact counts are listed in
  [validation.md](validation.md).
- The measured 1,000-fill market-sweep workload decreased from 1,046,750 ns to
  35,750 ns median after removing repeated queue-vector copies (about 29.3x for
  that one workload).
- The full benchmark table is in [benchmarking.md](benchmarking.md).
- No empirical market results, calibrated parameter estimates, or strategy
  profitability results were obtained.

## 14. Ablation studies actually completed

No Hawkes-kernel, taxonomy, mark-distribution, or market-model ablation study
has been completed. The matching queue-access change is a software performance
comparison, not a scientific model ablation. Synthetic Poisson comparison and
limited beta-misspecification likelihood checks exist, but no broad
multi-seed/power study has been completed.

## 15. Limitations

- No empirical feed, venue-specific configuration, or out-of-sample validation.
- No complete parameter-recovery, misspecification-sensitivity, or formal
  goodness-of-fit study.
- The decay profile grid is coarse; estimator model structures and inferential
  diagnostics have documented restrictions.
- Full event/trade/snapshot histories are retained in memory; no streaming or
  bounded-history mode exists.
- The synthetic adapter snapshots the full book at each event, so long/deep
  runs may incur substantial memory and copy cost.
- Replay ingestion does not reconstruct unobserved queue position or venue
  state.
- Benchmark workloads are short and from one machine; full Hawkes-adapter and
  analytics throughput/memory scaling have not been measured.

## 16. Future research directions

Priorities are to obtain and document suitable order-level historical feeds;
define venue-specific tick/lot/fee/latency/queue conventions; perform
time-separated calibration and validation; run multi-seed parameter-recovery
and misspecification studies; improve/compare estimator structures; add
streaming logs and bounded snapshot policies; and benchmark the full adapter,
analytics, parser, and strategy pipeline with allocation and memory profiling.

## 17. Reproduction instructions

From the repository root:

```sh
cmake --preset release
cmake --build --preset release --parallel 4
ctest --preset release
ctest --test-dir build/release --output-on-failure \
  -R "deterministic CSV fixture"
./build/release/lob_simulator \
  --events-out build/release/events.ndjson \
  --trades-out build/release/trades.ndjson \
  --manifest-out build/release/run_manifest.json \
  --analytics-out build/release/analytics.json
./build/release/lob_benchmarks
```

The executable runs simulation and analytics together; there is no standalone
analytics or calibration CLI. The checked-in `config/minimal_hawkes.json` is a
reference for fixed defaults and is not parsed by the executable. The
machine-readable output manifest records the actual run configuration.
