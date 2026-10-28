# Market Microstructure Analytics

This document specifies the mathematical definitions, accounting rules, edge-case handling, and statistical uncertainty methods for the market microstructure analytics module (`lob::analytics`).

The analytics module is strictly independent from the exchange matching engine. It consumes timestamped book snapshots, market event records, and trade executions without modifying exchange behavior.

---

## 1. Metrics & Mathematical Definitions

### 1.1 Best Bid, Best Ask, Spreads, and Midprice

Let $t$ index a book observation with resting bid price levels $\mathcal{B}(t) = \{(p_i^b, q_i^b)\}_{i=1}^{N_b}$ (sorted descending by price) and resting ask price levels $\mathcal{A}(t) = \{(p_j^a, q_j^a)\}_{j=1}^{N_a}$ (sorted ascending by price).

- **Best Bid**:
  $$P^b(t) = \begin{cases} p_1^b & \text{if } N_b > 0 \\ \text{undefined} & \text{if } N_b = 0 \end{cases}$$
- **Best Ask**:
  $$P^a(t) = \begin{cases} p_1^a & \text{if } N_a > 0 \\ \text{undefined} & \text{if } N_a = 0 \end{cases}$$
- **Bid-Ask Spread**:
  - In integer price ticks:
    $$S_{\text{ticks}}(t) = P^a(t) - P^b(t)$$
  - In currency units:
    $$S_{\text{currency}}(t) = S_{\text{ticks}}(t) \times c_{\text{tick}}$$
    where $c_{\text{tick}}$ is the configured currency units per tick (e.g. \$0.01/tick).
  - Time-Weighted Average Spread:
    $$\bar{S}_{\text{TW}} = \frac{\sum_{i=1}^{M-1} S(t_i) \Delta t_i}{\sum_{i=1}^{M-1} \Delta t_i}, \quad \Delta t_i = t_{i+1} - t_i$$
- **Midprice**:
  $$M(t) = \frac{P^b(t) + P^a(t)}{2}$$
- **Microprice (Volume-Weighted Midprice)**:
  $$M_{\text{micro}}(t) = \frac{P^b(t) Q^a_1(t) + P^a(t) Q^b_1(t)}{Q^b_1(t) + Q^a_1(t)}$$

**Edge cases**:
- If either side is missing ($N_b = 0$ or $N_a = 0$), spread, midprice, and microprice are undefined (`std::nullopt`).
- Intervals with undefined quotes are excluded from quote means and explicitly counted in `missing_bid_count`, `missing_ask_count`, and `empty_book_count`.

---

### 1.2 Displayed Depth & Order-Book Imbalance (OBI)

For a configured depth level $K \ge 1$:
- Displayed Bid Depth at level $K$:
  $$D^b_K(t) = \sum_{k=1}^{\min(K, N_b(t))} q_k^b(t)$$
- Displayed Ask Depth at level $K$:
  $$D^a_K(t) = \sum_{k=1}^{\min(K, N_a(t))} q_k^a(t)$$
- Total Depth at level $K$:
  $$D_K(t) = D^b_K(t) + D^a_K(t)$$
- **Order-Book Imbalance (OBI)**:
  $$I_K(t) = \frac{D^b_K(t) - D^a_K(t)}{D^b_K(t) + D^a_K(t)}$$

**Edge cases**:
- Range: $I_K(t) \in [-1.0, +1.0]$.
- If both sides are empty ($D_K(t) = 0$), the denominator is zero. The observation is recorded as undefined, excluded from mean imbalance, and counted in `zero_denominator_count`.
- If $D^b_K > 0$ and $D^a_K = 0$, $I_K = +1.0$.
- If $D^b_K = 0$ and $D^a_K > 0$, $I_K = -1.0$.

---

### 1.3 Order-Flow Imbalance (OFI)

Order-flow imbalance measures the net change in order flow at the best quotes across successive event or time intervals (Cont, Kukanov, and Stoikov, 2014). It must **not** be conflated with displayed-depth imbalance (OBI).

Between snapshot $n-1$ and snapshot $n$:
- Bid order flow $\Delta OF^b_n$:
  $$\Delta OF^b_n = \begin{cases} +Q^b_n & \text{if } P^b_n > P^b_{n-1} \\ Q^b_n - Q^b_{n-1} & \text{if } P^b_n = P^b_{n-1} \\ -Q^b_{n-1} & \text{if } P^b_n < P^b_{n-1} \end{cases}$$
- Ask order flow $\Delta OF^a_n$:
  $$\Delta OF^a_n = \begin{cases} +Q^a_n & \text{if } P^a_n < P^a_{n-1} \\ Q^a_n - Q^a_{n-1} & \text{if } P^a_n = P^a_{n-1} \\ -Q^a_{n-1} & \text{if } P^a_n > P^a_{n-1} \end{cases}$$
- **Net Event OFI**:
  $$\text{OFI}_n = \Delta OF^b_n - \Delta OF^a_n$$
- **Cumulative OFI**:
  $$\text{OFI}_{\text{cum}} = \sum_{n=1}^N \text{OFI}_n$$

**Edge cases**:
- If a side was empty at $n-1$ and becomes non-empty at $n$: $\Delta OF = +Q_n$.
- If a side was non-empty at $n-1$ and becomes empty at $n$: $\Delta OF = -Q_{n-1}$.
- If empty at both: $\Delta OF = 0$.

---

### 1.4 Trade Arrival Intensity & Overdispersion

For total observation duration $T = t_{\text{end}} - t_{\text{start}}$ (seconds):
- Trade Arrival Rate:
  $$\lambda_{\text{trade}} = \frac{N_{\text{trades}}}{T} \quad (\text{trades/sec})$$
- Volume Arrival Rate:
  $$\lambda_{\text{vol}} = \frac{\sum_{i=1}^{N_{\text{trades}}} Q_i}{T} \quad (\text{lots/sec})$$
- **Poisson Dispersion Ratio (Fano Factor)**:
  Partition $[0, T]$ into $B$ non-overlapping equal bins of width $\Delta t_{\text{bin}} = 1.0\,\text{s}$. Let $C_b$ be the trade count in bin $b$.
  $$D = \frac{\text{Var}(C)}{\mathbb{E}(C)} = \frac{\frac{1}{B-1}\sum_{b=1}^B (C_b - \bar{C})^2}{\frac{1}{B}\sum_{b=1}^B C_b}$$
  - For a homogeneous Poisson process, $D \approx 1$.
  - For self-exciting / clustered processes (Hawkes), $D > 1$ (overdispersion).

---

### 1.5 Order Submission & Cancellation Rates

For each event taxonomy category $c \in \{\text{LB}, \text{LA}, \text{MB}, \text{MS}, \text{CB}, \text{CA}\}$:
- Rate: $R_c = \frac{N_c}{T}$ (events/second).
- Aggregate Limit Submission Rate: $R_{\text{limit}} = \frac{N_{\text{LB}} + N_{\text{LA}}}{T}$.
- Aggregate Market Submission Rate: $R_{\text{market}} = \frac{N_{\text{MB}} + N_{\text{MS}}}{T}$.
- Aggregate Cancellation Rate: $R_{\text{cancel}} = \frac{N_{\text{CB}} + N_{\text{CA}}}{T}$.
- **Cancellation-to-Submission Ratio (CSR)**:
  $$\text{CSR} = \frac{N_{\text{cancel}}}{N_{\text{limit}}}$$
  (The denominator $N_{\text{limit}}$ is explicitly reported; if $N_{\text{limit}} = 0$, CSR is undefined).

---

### 1.6 Order Lifetime Distributions

For each resting limit order $j$:
- Submission timestamp: $t_j^{\text{sub}}$.
- Terminal timestamp: $t_j^{\text{term}}$ (if cancelled or fully filled).
- Lifetime:
  $$\tau_j = t_j^{\text{term}} - t_j^{\text{sub}} \quad (\text{seconds})$$
- Distributions reported separately for:
  1. Cancelled orders: $\tau \mid \text{status} = \text{Cancelled}$
  2. Filled orders: $\tau \mid \text{status} = \text{Filled}$
  3. All terminated orders
- **Right-censoring**: Orders still resting at simulation end $T$ are right-censored at $T - t_j^{\text{sub}}$ and tracked separately in `live_orders_censored_count`.

---

### 1.7 Trade-Size Distributions

For executed trades $i = 1, \dots, N_{\text{trades}}$:
- Traded lot quantity $Q_i$.
- Evaluated overall, and conditioned on aggressor side:
  - Buyer-initiated ($S_{\text{agg}} = \text{Buy}$)
  - Seller-initiated ($S_{\text{agg}} = \text{Sell}$)
- Reports mean, median, standard deviation, min, max, p25, p75, p95, p99.

---

### 1.8 Price-Return Distributions at Configurable Horizons

For each configured horizon $\tau \in \{\tau_1, \tau_2, \dots\}$:
- Midprices sampled at $t$ and future reference time $t + \tau$.
- **Tick Return**:
  $$\Delta M_\tau(t) = M(t + \tau) - M(t)$$
- **Simple Return**:
  $$r_\tau(t) = \frac{M(t + \tau) - M(t)}{M(t)}$$
- **Log Return**:
  $$r_{\log, \tau}(t) = \ln\left(\frac{M(t + \tau)}{M(t)}\right)$$
- Summary statistics include sample mean, standard deviation, sample skewness, and excess kurtosis.
- Right-censored intervals ($t + \tau > T$) or intervals where $M$ is undefined are excluded and recorded in `missing_or_censored_count`.

---

### 1.9 Realized Volatility at Configurable Intervals

For sampling interval $\delta$ (e.g. 0.1s, 1.0s) over duration $T$:
- Discretized grid times $t_k = k \delta$ for $k = 0, \dots, K$.
- Interval log return:
  $$r_k = \ln\left(\frac{M(t_k)}{M(t_{k-1})}\right)$$
- **Realized Variance**:
  $$RV_\delta = \sum_{k=1}^K r_k^2$$
- **Realized Volatility**:
  $$\sigma_{\text{realized}, \delta} = \sqrt{RV_\delta}$$
- **Annualized Volatility**:
  $$\sigma_{\text{ann}} = \sigma_{\text{realized}, \delta} \times \sqrt{\frac{T_{\text{annual}}}{T}}$$
  where $T_{\text{annual}}$ is the configured annual duration (default: $252 \times 6.5 \times 3600\,\text{s}$).
- Missing quote points are forward-filled, with the number of imputed intervals reported.

---

### 1.10 Market-Order Execution Costs

For each incoming market order $j$ with aggressor direction $S_{\text{agg}, j} \in \{+1 \text{ (Buy)}, -1 \text{ (Sell)}\}$:
- Executed trades $m = 1, \dots, M_j$ at prices $P_{j, m}$ and quantities $q_{j, m}$.
- Volume-Weighted Average Execution Price:
  $$\bar{P}_j = \frac{\sum_m P_{j, m} q_{j, m}}{\sum_m q_{j, m}}$$
- Pre-trade midprice $M(t_j^-)$ from the snapshot immediately prior to order arrival.
- **Effective Half-Spread**:
  $$\text{EffHalfSpread}_j = S_{\text{agg}, j} \times (\bar{P}_j - M(t_j^-)) \quad (\text{ticks})$$
- **Relative Effective Half-Spread**:
  $$\text{RelEffHalfSpread}_j = \frac{\text{EffHalfSpread}_j}{M(t_j^-)}$$
- **Price Slippage (walking the book)**:
  $$\text{Slippage}_j = S_{\text{agg}, j} \times (\bar{P}_j - P^{\text{opp}}(t_j^-)) \quad (\text{ticks})$$
  where $P^{\text{opp}}$ is the best opposing quote (best ask for buy, best bid for sell). If the order fills entirely at the best quote, slippage is 0.
- **Total Net Execution Cost**:
  $$\text{TotalNetCost} = \sum_j Q_j \left( \text{EffHalfSpread}_j \times c_{\text{tick}} + f_{\text{taker}} \right)$$

---

### 1.11 Short-Horizon Post-Trade Price Movements (Adverse Selection)

For each trade at price $P_{\text{trade}}$ where the passive resting order has side $S_{\text{passive}} \in \{+1 \text{ (Buy)}, -1 \text{ (Sell)}\}$:
- Pre-trade reference price: midprice $M(t^-)$ immediately prior to execution.
- Future reference price: midprice $M(t + \tau)$ at post-trade horizon $\tau$.
- Decomposition:
  1. **Spread Capture**:
     $$\text{SpreadCapture} = S_{\text{passive}} \times (M(t^-) - P_{\text{trade}}) \times c_{\text{tick}}$$
  2. **Subsequent Price Drift (Adverse Selection)**:
     $$\text{PriceDrift}(\tau) = S_{\text{passive}} \times (M(t^-) - M(t + \tau)) \times c_{\text{tick}}$$
  3. **Realized Spread**:
     $$\text{RealizedSpread}(\tau) = S_{\text{passive}} \times (M(t + \tau) - P_{\text{trade}}) \times c_{\text{tick}}$$
  4. **Accounting Identity**:
     $$\text{RealizedSpread}(\tau) \equiv \text{SpreadCapture} - \text{PriceDrift}(\tau)$$
  5. **Net Mark-to-Market Performance (including fees/rebates)**:
     $$\text{NetPnL}(\tau) = \text{RealizedSpread}(\tau) - f_{\text{maker}}$$
     where $f_{\text{maker}}$ is the maker fee per lot (negative indicates a liquidity rebate).

> [!IMPORTANT]
> The metric is labeled **Net Mark-to-Market Performance** rather than "profit" because it reflects short-horizon inventory revaluation and excludes funding, overnight carry, subsequent liquidation costs, and portfolio constraints.

---

### 1.12 Passive-Order Fill Statistics

For all resting limit orders submitted:
- $N_{\text{sub}}$: total submitted limit orders
- $N_{\text{filled}}$: fully filled orders
- $N_{\text{partial}}$: partially filled orders still live at horizon
- $N_{\text{canc\_unfilled}}$: cancelled without any execution
- $N_{\text{canc\_partial}}$: cancelled after partial execution
- $N_{\text{live}}$: remaining live orders
- **Order Fill Rate**:
  $$\text{OFR} = \frac{N_{\text{filled}} + N_{\text{partial}} + N_{\text{canc\_partial}}}{N_{\text{sub}}}$$
- **Complete Fill Rate**:
  $$\text{CFR} = \frac{N_{\text{filled}}}{N_{\text{sub}}}$$
- **Volume Fill Rate**:
  $$\text{VFR} = \frac{\sum Q_{\text{filled}}}{\sum Q_{\text{submitted}}}$$
- **Time to First Fill**: distribution of $t_{\text{first\_fill}} - t_{\text{sub}}$ for all executed orders.

---

## 2. Statistical Uncertainty & Autocorrelation (Newey-West HAC)

In high-frequency microstructure and Hawkes-driven data, observations are serially correlated (clustering, persistence of order flow and quotes).

Under positive serial correlation ($\rho_k > 0$), naive sample standard error:
$$SE_{\text{iid}} = \frac{s}{\sqrt{N}}$$
substantially underestimates the uncertainty of the sample mean.

### Newey-West HAC Variance Estimator
We compute Heteroskedasticity and Autocorrelation Consistent (HAC) standard error using the Bartlett kernel (Newey and West, 1987):
$$\hat{\gamma}_k = \frac{1}{N} \sum_{i=k+1}^N (X_i - \bar{X})(X_{i-k} - \bar{X})$$
$$\hat{V}_{\text{HAC}} = \frac{1}{N} \left[ \hat{\gamma}_0 + 2 \sum_{k=1}^L \left(1 - \frac{k}{L + 1}\right) \hat{\gamma}_k \right]$$
$$SE_{\text{HAC}} = \sqrt{\max\left(0, \hat{V}_{\text{HAC}}\right)}$$
where bandwidth $L = \min(hac\_max\_lags, N - 1)$.

### Effective Sample Size
When lag-1 autocorrelation $\rho_1 = \hat{\gamma}_1 / \hat{\gamma}_0 \in (-1, 1)$:
$$N_{\text{eff}} = N \frac{1 - \rho_1}{1 + \rho_1}$$
Both $SE_{\text{iid}}$, $SE_{\text{HAC}}$, and $N_{\text{eff}}$ are reported alongside every sample mean.
