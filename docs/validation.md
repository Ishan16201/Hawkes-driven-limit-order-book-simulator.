# Validation report

This report distinguishes software checks, mathematical checks, synthetic
experiments, empirical market validation, performance measurements, and open
limitations. It does not interpret synthetic self-consistency as evidence of
market realism.

## 1. Verified software correctness

Release tests include:

- Hand-calculated price-time priority, partial/multi-level fills, cancellation,
  market residual expiry, order-ID uniqueness, and trade-at-resting-price.
- A randomized independent reference-book comparison over four fixed seeds,
  600 operations per seed. The test compares fills, resting orders, best prices,
  per-price aggregate depth/FIFO order, cancellations, and internal invariants
  after each operation.
- Checked price-level quantity overflow with proof that the book remains
  unchanged.
- Deterministic replay of the small CSV fixture, including a partial fill and
  cancellation.
- Event-log/trade/manifest output checks, strategy accounting/risk/latency/causal
  interface tests, and existing analytics/ingestion tests.

The reference exchange is deliberately simple and independent for small
generated cases; the test does not prove correctness for all inputs or extreme
run lengths.

Final local test runs: Release 85/85 passed (0.43 s), Debug 85/85 passed
(1.71 s), and ASan/UBSan 85/85 passed (6.76 s). All three presets configured
and built successfully with Apple clang 21. The builds emitted no compiler
warnings.

## 2. Verified mathematical properties

Tests check:

- Multivariate recursive exponential state against direct historical
  summation.
- Exact exponential decay and pre-event intensity semantics.
- Branching stability based on the integrated `alpha` matrix, including a
  periodic matrix where unshifted power iteration could oscillate.
- Stationary mean for a one-dimensional model.
- Empty-history likelihood and analytic gradients against finite differences.
- Time-change residual means/variances and the two-sided KS statistic on a
  hand-calculated fixture.

The current synthetic rate check uses 24 independent seeds, each for 1,000
seconds, and checks whether the theoretical stationary mean lies within three
estimated standard errors of the across-seed mean. A separate five-seed
univariate fit uses known fixed beta and checks baseline/alpha recovery within
three across-seed standard errors; it also checks that a selected beta
misspecification has lower average likelihood. These fixed-seed tests are
limited synthetic checks, not a proof of stationarity or a comprehensive
misspecification study.

## 3. Synthetic statistical validation

The Hawkes generator is tested with a single-seed Poisson reduction, fixed-seed
reproducibility, finite nonnegative intensities, long-run rate, clustering
direction, and the multi-seed rate check above. The estimator has a synthetic
two-dimensional fit test against a Poisson baseline and a five-seed univariate
fixed-beta recovery/misspecification check. This is not a broad simulation
study; no general confidence intervals or power study have been produced.

Goodness-of-fit output reports residual counts and descriptive statistics.
Residuals are finite-sample and parameter-estimated diagnostics. The current
KS output is a statistic only; no KS p-value is reported. The estimator's
approximate likelihood-ratio p-value must not be interpreted as definitive
evidence because of nonnegative branching constraints and coarse decay
profiling.

## 4. Empirical market validation

**Incomplete.** No suitable real order-level or quote-level market dataset is
included or was used. Therefore there are no observed-vs-simulated comparisons
for spread/depth, order arrivals/lifetimes, cancellations, trade sizes,
short-horizon returns, flow clustering, or post-trade price movements. The
included deterministic fixture and Hawkes paths are synthetic software
fixtures, not substitutes for empirical data.

No in-sample/out-of-sample market calibration has been completed. Do not claim
empirical validation or calibration.

## 5. Performance results

Release-mode measurements and methodology are recorded in
[benchmarking.md](benchmarking.md). The measured matching-loop queue-copy
optimization improved the fixed 1,000-fill workload by about 29x in this
environment. The other reported results are single-machine, short local
benchmarks, not general capacity claims. The full Hawkes-adapter/analytics path
and memory scaling over long horizons remain unmeasured.

## 6. Sanitizers, memory, and numeric limits

Release and sanitizer test results are recorded above. The exchange checks
displayed-quantity addition and the Hawkes adapter rejects horizons that cannot
convert safely to nanoseconds. Seeded uniform draws use a representable open
interval. AddressSanitizer and
UndefinedBehaviorSanitizer are available through the `sanitize` preset on
supported Clang/GCC toolchains.

Still unresolved: long-duration event/history memory is unbounded; book
snapshots scale with both event count and book depth; no RSS growth test is
automated; and extreme near-critical/very-large-intensity numerical stress is
not exhaustive. `QuantityLots` aggregate and total-summary addition are checked,
but identifier counters are not stress-tested near their integer limits.

## 7. Final audit notes

- The README and design introduction no longer claim the project is an empty
  scaffold.
- The calibration guide now distinguishes implemented coarse decay profiling
  from continuous beta estimation; unsupported parent-shared decay fails
  explicitly.
- Event and trade serialization now contains enough command/fill fields for
  audit and reconciliation.
- No historical market data, secrets, or generated run output is intended to
  be committed. Build/test output belongs under ignored `build/`.
- The working tree is presented by this environment as untracked files, so a
  conventional Git diff against a tracked baseline is unavailable. Files were
  edited in place and unrelated state was not removed.

The documented synthetic CLI produced 72 Hawkes-event lines and 10 fill lines.
Two independent invocations produced byte-identical event, trade, manifest,
and analytics files; both JSON outputs parsed successfully. The checked-in
example configuration also parsed as JSON.

See [research_report.md](research_report.md) for the research question,
assumptions, actual output, and reproduction protocol.
