# Strategy and Execution Evaluation Framework

This document specifies the strategy and execution-evaluation framework for the Hawkes-driven limit order book simulator.

The framework adheres to the system invariants defined in `AGENTS.md` and `docs/design.md`:
* **Complete decoupling**: Strategy logic is strictly isolated from exchange matching logic, the Hawkes stochastic arrival process, historical ingestion, and the analytics engine.
* **Causal execution context**: Strategies receive only information strictly available at the current simulated time; future timeline events, future book states, future trades, and random draws are strictly unobservable.
* **Deterministic integer accounting**: Prices are tracked as integer ticks, quantities as integer lots, time as integer nanoseconds, and financial accounting is fully reconciled with explicit fills, cash movements, and fee schedules.

---

## 1. Architecture and Interface Decoupling

The execution framework consists of four primary components:

```
+-------------------------------------------------------------+
|                      Strategy                               |
|   (Receives Causal StrategyContext, Submits Via Emitter)    |
+-------------------------------------------------------------+
                              |
                     StrategyActionEmitter
                              |
                              v
+-------------------------------------------------------------+
|                     OrderBroker                             |
|  - Pre-trade risk limit validation                          |
|  - Outbound/Inbound latency queuing                         |
|  - Event-driven cash & average-cost position accounting     |
|  - Mark-to-market tracking & drawdown monitoring            |
+-------------------------------------------------------------+
                              |
               exchange::MatchingEngine Commands
                              |
                              v
+-------------------------------------------------------------+
|                 Matching Engine & Order Book                |
|            (Deterministic execution & trade generation)      |
+-------------------------------------------------------------+
```

### 1.1 Strategy Interface (`Strategy`)

Strategies inherit from the pure virtual interface `lob::strategy::Strategy`:

```cpp
class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual void on_start(const StrategyContext& ctx, StrategyActionEmitter& emitter) = 0;
  virtual void on_quote(const StrategyContext& ctx, StrategyActionEmitter& emitter) = 0;
  virtual void on_trade(const StrategyContext& ctx, const StrategyMarketTrade& trade, StrategyActionEmitter& emitter) = 0;
  virtual void on_execution_report(const StrategyContext& ctx, const StrategyExecutionReport& report, StrategyActionEmitter& emitter) = 0;
  virtual void on_timer(const StrategyContext& ctx, StrategyActionEmitter& emitter) = 0;
  virtual void on_stop(const StrategyContext& ctx, StrategyActionEmitter& emitter) = 0;
  [[nodiscard]] virtual std::string name() const = 0;
};
```

The interface uses strategy-owned `StrategyBookSnapshot`, `StrategyMarketTrade`,
and `StrategyOrderStatus` value types. Public trade observations omit exchange
order identifiers. Only the broker/evaluator adapter depends on matching-engine
types; strategy implementations do not.

### 1.2 Lookahead Prevention (`StrategyContext`)

A strategy cannot inspect future events or future book states. It only accesses the read-only `StrategyContext`:
* `current_time_ns()`: Simulated time of the current event boundary.
* `best_bid()` and `best_ask()`: Prevailing top-of-book prices as of `current_time_ns()`.
* `midprice()`: Arithmetic midprice $\frac{P_{\text{bid}} + P_{\text{ask}}}{2}$ at `current_time_ns()`.
* `current_book()`: Central order book snapshot at `current_time_ns()`.
* `portfolio()`: Read-only current position, cash, fees, realized, and unrealized P&L.
* `open_orders()`: Current active resting orders belonging to this strategy.

Neither the Hawkes process instance, random number generator, future event queue, nor historical replay buffer are accessible through this context.
The evaluator advances in timestamp order. Outbound commands are dispatched at
their exact maturity time before later market events; execution reports become
visible only after inbound latency. At equal timestamps, timers precede external
events, generated market events retain their sequence, and already-due reports
and commands are resolved before the next external event. Snapshot caches are
invalidated after same-time book changes.

---

## 2. Event-Driven Portfolio Accounting

The broker performs continuous, event-driven accounting upon every fill and market price update.

### 2.1 Cash and Position Updates

Let an executed fill have:
* Side $S \in \{+1 \text{ (Buy)}, -1 \text{ (Sell)}\}$
* Fill price $P_{\text{fill}}$ (in ticks)
* Fill quantity $Q_{\text{fill}}$ (in lots)
* Tick currency scale $c_{\text{tick}}$
* Fee paid $F$

The cash balance updates explicitly as:
$$\Delta \text{Cash} = -S \times (Q_{\text{fill}} \times P_{\text{fill}} \times c_{\text{tick}}) - F$$

The inventory position updates as:
$$\text{Position}_{t} = \text{Position}_{t-} + S \times Q_{\text{fill}}$$

### 2.2 Volume-Weighted Average Entry Price ($\bar{P}_{\text{entry}}$)

* **Opening position** ($\text{Position}_{t-} = 0$):
  $$\bar{P}_{\text{entry}} = P_{\text{fill}}$$
* **Adding to position** ($\text{sgn}(\text{Position}_{t-}) = S$):
  $$\bar{P}_{\text{entry}} \leftarrow \frac{|\text{Position}_{t-}| \times \bar{P}_{\text{entry}} + Q_{\text{fill}} \times P_{\text{fill}}}{|\text{Position}_{t-}| + Q_{\text{fill}}}$$
* **Closing or reducing position** ($\text{sgn}(\text{Position}_{t-}) \neq S$):
  The average entry price of the remaining inventory is unchanged.
* **Flipping position through zero** ($Q_{\text{fill}} > |\text{Position}_{t-}|$):
  The closing portion ($|\text{Position}_{t-}|$) is realized at $\bar{P}_{\text{entry}}$, and the remaining oversold/overbought portion establishes a new position with entry price $P_{\text{fill}}$.

### 2.3 Realized and Unrealized P&L

When closing $Q_{\text{close}} = \min(Q_{\text{fill}}, |\text{Position}_{t-}|)$ lots with position sign $S_{\text{pos}} \in \{+1, -1\}$:
$$\Delta \text{GrossRealizedPnL} = S_{\text{pos}} \times (P_{\text{fill}} - \bar{P}_{\text{entry}}) \times Q_{\text{close}} \times c_{\text{tick}}$$

Unrealized profit/loss is marked strictly against the current prevailing midprice $M_t$:
$$\text{UnrealizedPnL}(t) = \text{Position}_t \times (M_t - \bar{P}_{\text{entry}}) \times c_{\text{tick}}$$

### 2.4 Accounting Invariant and Reconciliation

At all times $t$, total net portfolio equity reconciles identically:
$$\text{Equity}(t) = \text{Cash}(t) + \text{Position}_t \times M_t \times c_{\text{tick}}$$
$$\text{NetTotalPnL}(t) = \text{GrossRealizedPnL}(t) + \text{UnrealizedPnL}(t) - \text{TotalFees}(t) \equiv \text{Equity}(t) - \text{InitialCash}$$

Initial cash is zero in this evaluator. The broker marks inventory to the latest
midprice observable at or before the current simulated time. If either side of
the book is empty, the last valid mark is retained; no future price is used.
`PortfolioState::total_equity_currency()` is cash plus marked inventory value
(fees are already included in cash), and drawdown is measured against peak
marked equity. Unfilled or cancelled orders never alter cash, position, fees,
or trade counts.

---

## 3. Transaction Costs and Latency Simulation

Transaction costs and latency parameters are configured in `FeeSchedule` and `LatencyModel`:

```cpp
struct FeeSchedule {
  double maker_fee_per_lot{0.0};      // Negative indicates exchange liquidity rebate
  double taker_fee_per_lot{0.0};      // Positive indicates exchange fee
  double commission_per_order{0.0};   // Charged once, on an order's first fill
};

struct LatencyModel {
  core::DurationNs outbound_delay_ns{0}; // Strategy decision -> Exchange arrival
  core::DurationNs inbound_delay_ns{0};  // Exchange fill -> Strategy receipt
};
```

* **No Double Counting**: Fill prices are determined deterministically by matching against the resting queue. Bid-ask spread and market impact are natively reflected in the executed prices. Fee schedules apply only explicit trading fees, rebates, and brokerage commissions.
* **Commission policy**: A fixed commission is charged once, on the first partial or full fill of an order; a rejected, unfilled, or cancelled-without-fill order incurs no commission. Maker/taker fees or rebates apply per executed lot.
* **Separation of Delay**: Orders submitted at $t$ mature for matching at $t + \Delta t_{\text{outbound}}$. Market movements that occur between $t$ and $t + \Delta t_{\text{outbound}}$ alter the book state before the strategy's order arrives. Fill reports are withheld until $t_{\text{fill}} + \Delta t_{\text{inbound}}$.
* **Impact/slippage**: Each fill records signed execution cost against the pre-execution midprice. This descriptive metric is not an extra cash charge. The simulated book's actual fill prices include spread crossing and depth consumption, so the evaluator adds no second impact model. Parent-order implementation shortfall against arrival midprice captures spread, market movement, and depth impact together; these components are not independently estimated.
* **Risk**: Position limits include currently open and latency-pending order quantities using worst-case directional exposure. A drawdown at or above the configured limit rejects new orders until the run ends; resting orders are not silently cancelled.

---

## 4. Baseline Reference Policies

The framework implements four reference policies in `baseline_strategies.hpp`:

1. **`NoTradeStrategy`**: Submits zero orders. Establishes the zero-turnover, zero-cost, zero-PnL benchmark.
2. **`PassiveLimitExecutionStrategy`**: Quotes resting limit orders at the prevailing best quote to capture the spread and maker rebates. Updates price if the quote moves.
3. **`MarketOrderExecutionStrategy`**: Submits an immediate aggressive market order for the target size to execute without delay.
4. **`TWAPExecutionStrategy`**: Slices a fixed parent order of size $Q$ into at most $N$ integer-lot tranches of size at most $\lceil Q/N \rceil$; the final tranche can be smaller. The first is submitted at start; later slices are scheduled uniformly through $T$, including the horizon. Timer callbacks do not cause early dispatch, and total submitted size cannot exceed $Q$.

> [!NOTE]
> These policies are reference benchmarks for methodology validation, not claims of algorithmic trading profitability.

---

## 5. Evaluation Framework

The evaluator separates **Market-Making / Inventory Strategies** from **Parent-Order Execution Strategies**:

### 5.1 Market-Making Metrics (`MarketMakingMetrics`)
* Gross & Net P&L (currency)
* Turnover & Total Fees Paid
* Signed execution cost versus contemporaneous pre-execution midprice (reported separately; not a cash charge)
* Passive vs. Aggressive Fill Ratio ($\frac{N_{\text{passive}}}{N_{\text{total}}}$)
* Maximum Inventory Exposure ($\max_t |\text{Position}_t|$)
* Distinct-order fill rate and the final remaining open-order list
* Peak-to-Trough Drawdown

### 5.2 Parent-Order Execution Metrics (`ParentOrderExecutionMetrics`)
* Target vs. Executed Quantity & Completion Rate ($\frac{Q_{\text{exec}}}{Q_{\text{target}}}$)
* Arrival Midprice ($M_0$)
* Execution Volume-Weighted Average Price ($\text{VWAP} = \frac{\sum P_i Q_i}{\sum Q_i}$)
* **Implementation Shortfall (IS)**:
  $$\text{IS}_{\text{ticks}} = S \times (\text{VWAP} - M_0)$$
  $$\text{IS}_{\text{currency}} = \text{IS}_{\text{ticks}} \times Q_{\text{exec}} \times c_{\text{tick}}$$
* Total Execution Cost = $\text{IS}_{\text{currency}} + \text{TotalFees}$

### 5.3 Multi-Run Uncertainty Estimation (`StrategyEvaluator::run_multiple`)

Evaluates performance across randomized seeds:
$$\bar{X} = \frac{1}{N}\sum_{i=1}^N X_i, \quad s = \sqrt{\frac{1}{N-1}\sum_{i=1}^N (X_i - \bar{X})^2}, \quad \text{SE} = \frac{s}{\sqrt{N}}$$
Summaries are formatted into human-readable tables and machine-readable JSON.
The mean, sample standard deviation, and standard error of net total P&L are
reported across runs; means for gross P&L, turnover, fees, execution cost,
fill rate, inventory exposure, and drawdown are also included. Parent-order
completion, shortfall, and total cost are reported
separately from market-making P&L; neither benchmark constitutes evidence of
profitability or empirical calibration.
