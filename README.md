# Hawkes-Driven Limit Order Book Simulator

A research-oriented C++20 simulator for studying how clustered order flow
interacts with a deterministic limit order book. It combines a seedable
multivariate Hawkes-process generator, a synthetic event-to-order adapter,
price-time-priority matching, historical event-ingestion primitives,
descriptive analytics, a Hawkes estimation API, and a causal strategy and
execution evaluator.

The exchange is deliberately independent of the Hawkes process, historical
parser, estimator, analytics, and strategy code. Each component produces or
consumes typed events; the exchange itself only processes submitted orders and
cancellations.

> **Research status:** The example parameters and mark distributions are
> illustrative, not calibrated. No suitable historical market data is included
> or was used, so empirical market validation has not been performed. Synthetic
> tests validate software behavior and selected mathematical properties; they
> do not establish market realism or strategy profitability.

## What this project models

The simulator has two distinct layers:

1. **Order-flow model:** A multivariate Hawkes process determines when an event
   occurs and which event type occurs (for example, a buy limit submission or
   a sell cancellation).
2. **Exchange model:** An adapter assigns event-specific prices, sizes, and
   cancellation targets, then submits commands to a deterministic limit order
   book. The matching engine decides whether the command fills or rests.

This separation matters: a Hawkes event is not itself a trade. A generated
market-buy event can produce no fill if the ask side is empty; a limit order
may rest without executing; and only matching-engine fills are executions.
Strategies, in turn, see current-time market and portfolio information and
submit orders through a broker rather than controlling the generator or
exchange internals.

## Core concepts and mathematics

### Multivariate Hawkes process

A point process represents events by their event times and types. Write
`N_j(t)` for the number of events of type `j` observed by time `t`. A
Hawkes process makes the instantaneous event rate depend on past events.
For each possible event type `i`, its conditional intensity is

```text
lambda_i(t) = mu_i
            + sum_j sum_{k: type(k)=j, t_k<t}
                alpha_ij * beta_ij * exp(-beta_ij * (t - t_k)).
```

The intensity `lambda_i(t)` is a rate, not a probability: over a sufficiently
short interval `dt`, the chance of a type-`i` event is approximately
`lambda_i(t) * dt`. The history sum is over events strictly before `t`; the
event being recorded does not excite itself before its own arrival.

The parameters have distinct roles and units:

| Symbol | Meaning | Unit |
|---|---|---|
| `t`, `T` | Event time and observation horizon | seconds |
| `mu_i` | Background rate of event type `i` | events/second |
| `alpha_ij` | Integrated excitation from a type-`j` event to type `i` | dimensionless |
| `beta_ij` | How quickly that excitation decays | inverse seconds |
| `lambda_i(t)` | Current conditional rate for type `i` | events/second |

The kernel is normalized so that

```text
phi_ij(u) = alpha_ij * beta_ij * exp(-beta_ij * u),  u >= 0
integral_0^infinity phi_ij(u) du = alpha_ij.
```

So `alpha_ij` controls the total expected direct excitation contributed by one
parent event; `beta_ij` controls its timing. A larger beta concentrates the
same integrated excitation closer to the parent event. Rows of the `alpha`
and `beta` matrices are child types, columns are parent types.

The configured event types are:

| Label | Meaning |
|---|---|
| `LB` | Limit buy submission |
| `LA` | Limit sell submission |
| `CB` | Buy-side cancellation |
| `CA` | Sell-side cancellation |
| `MB` | Aggressive buy / market buy |
| `MS` | Aggressive sell / market sell |

These six categories and their excitation relationships are modeling choices,
not claims about a particular venue.

### Branching, stability, and mean rates

The dimensionless matrix `A = [alpha_ij]` is called the branching matrix. Its
entry `alpha_ij` is the expected number of direct type-`i` offspring associated
with a type-`j` parent in the branching interpretation of a linear Hawkes
process.

For the nonnegative kernels used here, the subcritical stationarity condition
is

```text
rho(A) < 1
```

where `rho` is the spectral radius (largest absolute eigenvalue). Under this
condition, the theoretical stationary mean intensity is

```text
bar_lambda = (I - A)^(-1) * mu.
```

This is a theoretical long-run mean, not a guarantee that a finite simulation
will produce exactly that rate. Near the stability boundary, long-run rates and
clusters can become large and more sensitive to parameters. The implementation
validates the branching matrix and reports its spectral-radius diagnostics.

### Efficient state update and event generation

Directly summing every prior event at every time step would be expensive. With
exponential kernels, the process can instead maintain an excitation state for
each child-parent pair:

```text
x_ij(t) = sum_{k: type(k)=j, t_k<t}
          alpha_ij * beta_ij * exp(-beta_ij * (t - t_k))
lambda_i(t) = mu_i + sum_j x_ij(t).
```

Between candidate times the state decays exactly by `exp(-beta_ij * dt)`. When
a type-`j` event is accepted, it adds `alpha_ij * beta_ij` to each child
channel's state. The simulator uses Ogata thinning: it samples a candidate
wait from the current total intensity, advances the decaying state, accepts or
rejects the candidate against the updated intensity, and selects an accepted
event type in proportion to its intensity. For nonnegative exponential
kernels, the current total rate bounds future rates between accepted events
because excitation decays between events.

The random engine is seeded. The same seed and configuration reproduce a run
with the same executable and C++ standard-library implementation; standard
distribution implementations need not be bit-identical across library
vendors.

### Hawkes likelihood and estimation

For observed events `(t_k, m_k)` over `[0,T]`, the point-process log-likelihood
is

```text
log L = sum_k log(lambda_{m_k}(t_k)) - sum_i Lambda_i(T)
Lambda_i(T) = integral_0^T lambda_i(t) dt
            = mu_i*T
              + sum_j alpha_ij * sum_{k: m_k=j}
                  (1 - exp(-beta_ij * (T - t_k))).
```

The first term rewards intensity assigned to observed events; the compensator
term penalizes models that assign too much total event rate over the whole
observation window. This also handles a history with no events: its log
likelihood is `-T * sum_i mu_i`, rather than zero by convention.

The in-process `lob::estimation::HawkesEstimator` evaluates this likelihood,
uses analytic gradients for the baselines and branching coefficients, and
compares Hawkes fits with an independent Poisson benchmark. Depending on the
selected model structure, it constrains cross-excitation or event dimension.
Decay is either held fixed or selected from a small common-decay candidate
grid; it is **not** optimized continuously. Nonnegative coefficients and a
subcritical stability bound constrain fitting. The optimizer uses seeded
restarts and projected gradient ascent.

These restrictions mean a fit is a constrained, coarse-profile fit, not a
general unconstrained maximum-likelihood estimate of every `alpha_ij` and
`beta_ij`. The fitter's likelihood-ratio tail is approximate, and standard
errors are asymptotic diagnostics. Goodness-of-fit output includes
compensator-time residual summaries and a two-sided KS statistic, but no KS
p-value. A failure to reject a goodness-of-fit null hypothesis would not prove
that a model is correct. There is currently no calibration command-line tool.
See [Calibration](docs/calibration.md) for precise restrictions and caveats.

### Exchange: orders, priority, and fills

The exchange is a single-instrument continuous limit order book:

- Bids are ordered from highest price to lowest; asks from lowest to highest.
- At each price, accepted orders are queued FIFO (first in, first out).
- A buy can match the best ask if its limit price is at least that ask; a sell
  can match the best bid if its limit price is no greater than that bid.
- Trades execute at the **resting order's price**. An incoming order may fill
  against multiple orders and price levels.
- A partial fill reduces remaining quantity without changing the resting
  order's queue position. A fully filled order leaves the book.
- A nonmarketable limit order rests. An unfilled market-order remainder
  expires; market orders never rest.
- Cancellation removes the entire remaining quantity of a live order.

The best bid and ask define the quoted spread and midpoint:

```text
spread = best_ask - best_bid
midprice = (best_ask + best_bid) / 2
```

Prices are integer ticks and quantities are integer lots. A fill is an executed
trade; an order being submitted, resting, or cancelled is not. The core
matching engine does not model venue fees, hidden liquidity, auctions,
participant identity, or exchange latency.

### From event type to exchange command

The market adapter translates a Hawkes type into an order or cancellation. It
uses separate mark settings for limit-price placement, order sizes,
cancellation targeting, and initial book construction. In the example, limit
prices are placed relative to a current best quote or fallback reference,
sizes are sampled as integer lots from configured ranges, market orders are
immediate, and the default cancellation policy targets the FIFO-front order at
the best price on the selected side. These mark rules are separate from the
Hawkes timing/type model; they are illustrative synthetic assumptions.

### Strategies, accounting, and transaction costs

The strategy interface receives a causal snapshot: the current book, current
time, current portfolio, current open orders, and reports that have arrived by
that time. It does not expose the future event queue, future book state, or
random-number generator. Orders pass through a broker that applies risk limits,
latency, fills, and portfolio accounting.

For a fill with signed side `s` (`+1` for buy, `-1` for sell), price `P` in
ticks, quantity `Q` in lots, tick value `c`, and fee `F`, the broker updates:

```text
cash change     = -s * Q * P * c - F
position change =  s * Q
```

Realized P&L is calculated when a fill closes existing inventory; unrealized
P&L marks remaining inventory to the latest observable midpoint:

```text
unrealized P&L = position * (current_midprice - average_entry_price) * c
equity         = cash + position * current_midprice * c
```

If one side of the book is empty, the broker retains the last valid mark rather
than using a future price. Unfilled or cancelled orders do not change cash,
position, fees, or trade counts.

Maker/taker fees and rebates are charged per executed lot; a configured
per-order commission is charged on the first fill. The actual simulated fill
price already reflects spread crossing and depth consumed. Therefore the
evaluator reports signed execution cost/slippage descriptively and does not
charge an additional impact model on top of the fill price. Parent-order
implementation shortfall is measured relative to arrival midpoint and is
reported separately from market-making P&L.

Reference policies include no-trade, passive limit execution, market-order
execution, and TWAP-style parent-order slicing. They are benchmarks for
evaluation, not evidence of profitability. Multi-run evaluation reports
variation across runs; market-making/inventory results are kept distinct from
parent-order execution costs.

## Units and conventions

| Quantity | Representation |
|---|---|
| Exchange prices | Signed 64-bit integer ticks |
| Exchange order quantities | Unsigned 64-bit integer lots |
| Exchange timestamps | Signed 64-bit integer nanoseconds |
| Hawkes event times and horizon | Floating-point seconds |
| Hawkes baseline rates | Events per second |
| Hawkes decay rates | Inverse seconds |
| Hawkes branching coefficients | Dimensionless |

When Hawkes event times are passed to the exchange, seconds are floored to
integer nanoseconds. Distinct Hawkes events can consequently map to the same
exchange timestamp; generated order is preserved for them. Tick currency value
and lot/share multipliers are instrument metadata and are not inferred by the
matching engine.

## Build and test

Requirements: CMake 3.25+, a C++20 compiler, and Git. The tests use Catch2
v3.8.0 pinned to an immutable commit via CMake `FetchContent`; the first
configure may need network access to fetch Catch2. The core runtime does not
require Python or network access.

```sh
cmake --preset release
cmake --build --preset release --parallel 4
ctest --preset release
```

For a debug build or an AddressSanitizer/UndefinedBehaviorSanitizer build:

```sh
cmake --preset dev
cmake --build --preset dev --parallel 4
ctest --preset dev

cmake --preset sanitize
cmake --build --preset sanitize --parallel 4
ctest --preset sanitize
```

The hand-authored replay fixture exercises parsing, partial execution, and
cancellation:

```sh
ctest --test-dir build/release --output-on-failure \
  -R "deterministic CSV fixture"
```

## Run a reproducible synthetic experiment

Build the Release preset, then run:

```sh
./build/release/lob_simulator \
  --events-out build/release/events.ndjson \
  --trades-out build/release/trades.ndjson \
  --manifest-out build/release/run_manifest.json \
  --analytics-out build/release/analytics.json
```

This runs the illustrative six-type Hawkes process, event-to-order adapter,
matching engine, and analytics in one executable. The fixed seed, finite
10-second horizon, Hawkes parameters, mark distributions, initial book, and
counts are recorded in the machine-readable manifest. The four outputs are:

| Output | Contents |
|---|---|
| `events.ndjson` | Hawkes event type/time, pre-event intensities, mapped outcome and order details |
| `trades.ndjson` | Individual exchange fills, including price, quantity, side, and order IDs |
| `run_manifest.json` | Run seed, model and mark settings, initial book, units, and run counts |
| `analytics.json` | Descriptive microstructure analytics and their configuration |

Outputs are directed to the ignored `build/` tree. The JSON file
[`config/minimal_hawkes.json`](config/minimal_hawkes.json) documents the example
parameters; it is a reference file and is **not** parsed by the executable.
There is no standalone synthetic-data generator, calibration, or analytics
command-line executable. Estimation is available through the C++ API and tests;
the simulator CLI performs simulation and analytics together.

## Benchmarks

```sh
./build/release/lob_benchmarks
```

The harness reports five-run medians for best-price lookup, order insertion
and cancellation, matching, Hawkes updates and event-time simulation,
serialization, and deterministic simulation. Recorded Release results and
methodology—including hardware, compiler, and workload definitions—are in
[Benchmarking](docs/benchmarking.md). They are local measurements, not
portable performance claims.

## Validation and known limitations

The test suite includes hand-calculated matching and accounting cases,
randomized small-book comparisons against a separate reference implementation,
deterministic replay, Hawkes recursion and stability checks, likelihood and
residual diagnostics, synthetic rate checks, and limited multi-seed parameter
recovery checks. Build presets also cover Debug and ASan/UBSan.

The following limitations are material:

- **No empirical market validation:** no suitable historical order-level or
  quote-level dataset has been used. Synthetic self-validation is not a
  substitute.
- **Illustrative parameters:** the checked-in run is not calibrated, and its
  output should not be interpreted as a market forecast or profitability
  result.
- **Coarse estimator:** common decay is fixed or selected from a small
  `{1, 5, 10}` inverse-second grid; it is not continuously estimated. Some
  model structures and inferential diagnostics have documented restrictions.
- **No venue certification:** historical parsing is a basic adapter. Feed
  conventions, missing state, queue position, and venue-specific matching
  behavior require separate validation.
- **Memory grows with run length:** exchange histories and adapter outputs are
  retained, and a full book snapshot is stored for each generated event. There
  is no streaming or bounded-history mode yet.
- **Incomplete end-to-end profiling:** the full Hawkes-adapter/analytics path,
  long-run memory scaling, and parser throughput have not been benchmarked.

See the [validation report](docs/validation.md) for verified results and
unresolved issues. Do not interpret a failure to reject a statistical
goodness-of-fit null hypothesis as proof that the model is correct.

## Documentation

- [Architecture and component boundaries](docs/architecture.md)
- [Mathematical model and simulation choices](docs/mathematical_model.md)
- [Exchange rules and invariants](docs/exchange_rules.md)
- [Data formats and output schema](docs/data_schema.md)
- [Calibration implementation and caveats](docs/calibration.md)
- [Strategy and execution evaluation](docs/strategy.md)
- [Validation report](docs/validation.md)
- [Benchmark methodology and results](docs/benchmarking.md)
- [Research report](docs/research_report.md)
- [Accepted system design](docs/design.md)
