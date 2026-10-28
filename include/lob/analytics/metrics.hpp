#pragma once

#include "lob/analytics/types.hpp"
#include "lob/core/types.hpp"
#include "lob/exchange/order_book.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lob::analytics {

// ---------------------------------------------------------------------------
// 1, 2, 3: Quotes, Spreads, and Midprice
// ---------------------------------------------------------------------------
struct QuoteMetrics final {
  std::size_t total_observations{0};
  std::size_t valid_two_sided_count{0};
  std::size_t missing_bid_count{0};
  std::size_t missing_ask_count{0};
  std::size_t empty_book_count{0};

  SampleStats best_bid_ticks;
  SampleStats best_ask_ticks;

  SampleStats spread_ticks;
  SampleStats spread_currency;
  double time_weighted_spread_ticks{0.0};
  double time_weighted_spread_currency{0.0};

  SampleStats midprice_ticks;
  SampleStats midprice_currency;
  SampleStats microprice_ticks;  // Top-of-book volume-weighted midprice
  double time_weighted_midprice_ticks{0.0};
};

// ---------------------------------------------------------------------------
// 4, 5: Displayed Depth & Order-Book Imbalance
// ---------------------------------------------------------------------------
struct LevelDepthStats final {
  std::size_t level_k{1};
  SampleStats bid_depth_lots;
  SampleStats ask_depth_lots;
  SampleStats total_depth_lots;

  /// Order-book imbalance: I_k = (Bid_k - Ask_k) / (Bid_k + Ask_k) in [-1, +1].
  SampleStats imbalance;
  std::size_t zero_denominator_count{0};
  std::size_t valid_imbalance_count{0};
};

struct DepthMetrics final {
  std::vector<LevelDepthStats> levels;
};

// ---------------------------------------------------------------------------
// 6: Order-Flow Imbalance (OFI)
// ---------------------------------------------------------------------------
struct OrderFlowImbalanceMetrics final {
  std::size_t total_event_transitions{0};
  std::int64_t cumulative_ofi_lots{0};
  SampleStats event_ofi_lots;

  double bin_size_seconds{1.0};
  std::size_t bin_count{0};
  SampleStats binned_ofi_lots;
};

// ---------------------------------------------------------------------------
// 7: Trade Arrival Intensity
// ---------------------------------------------------------------------------
struct TradeIntensityMetrics final {
  double duration_seconds{0.0};
  std::size_t trade_count{0};
  core::QuantityLots total_volume_lots{0};

  double trade_arrival_rate_per_sec{0.0};
  double volume_arrival_rate_per_sec{0.0};

  /// Fano factor (variance-to-mean ratio of trade counts in 1s bins).
  /// D > 1 indicates clustering/overdispersion beyond Poisson.
  double dispersion_ratio{0.0};
  std::size_t dispersion_bin_count{0};
};

// ---------------------------------------------------------------------------
// 8: Order Submission & Cancellation Rates
// ---------------------------------------------------------------------------
struct SubmissionAndCancellationMetrics final {
  double duration_seconds{0.0};

  double lb_rate_per_sec{0.0};
  double la_rate_per_sec{0.0};
  double mb_rate_per_sec{0.0};
  double ms_rate_per_sec{0.0};
  double cb_rate_per_sec{0.0};
  double ca_rate_per_sec{0.0};

  double limit_submission_rate_per_sec{0.0};
  double market_submission_rate_per_sec{0.0};
  double cancellation_rate_per_sec{0.0};

  std::size_t total_limit_submissions{0};
  std::size_t total_cancellations{0};
  double cancel_to_limit_ratio{0.0};
  bool cancel_to_limit_ratio_defined{false};
};

// ---------------------------------------------------------------------------
// 9: Order Lifetime Distributions
// ---------------------------------------------------------------------------
struct OrderLifetimeMetrics final {
  SampleStats cancelled_order_lifetimes_s;
  SampleStats filled_order_lifetimes_s;
  SampleStats all_terminal_order_lifetimes_s;
  std::size_t live_orders_censored_count{0};
};

// ---------------------------------------------------------------------------
// 10: Trade-Size Distributions
// ---------------------------------------------------------------------------
struct TradeSizeMetrics final {
  SampleStats overall_trade_sizes_lots;
  SampleStats buyer_initiated_sizes_lots;
  SampleStats seller_initiated_sizes_lots;
  core::QuantityLots buyer_volume_lots{0};
  core::QuantityLots seller_volume_lots{0};
};

// ---------------------------------------------------------------------------
// 11: Price-Return Distributions at Configurable Horizons
// ---------------------------------------------------------------------------
struct HorizonReturnStats final {
  double horizon_seconds{0.0};
  std::size_t valid_observation_count{0};
  std::size_t missing_or_censored_count{0};

  SampleStats tick_returns;    // M(t + tau) - M(t) in ticks
  SampleStats simple_returns;  // (M(t + tau) - M(t)) / M(t)
  SampleStats log_returns;     // ln(M(t + tau) / M(t))

  double skewness{0.0};
  double kurtosis{0.0};
};

struct PriceReturnMetrics final {
  std::vector<HorizonReturnStats> horizons;
};

// ---------------------------------------------------------------------------
// 12: Realized Volatility at Configurable Sampling Intervals
// ---------------------------------------------------------------------------
struct IntervalRealizedVolatility final {
  double interval_seconds{0.0};
  std::size_t interval_count{0};
  std::size_t imputed_or_missing_intervals{0};

  double realized_variance{0.0};
  double realized_volatility{0.0};
  double annualized_volatility{0.0};
};

struct RealizedVolatilityMetrics final {
  std::vector<IntervalRealizedVolatility> intervals;
};

// ---------------------------------------------------------------------------
// 13: Market-Order Execution Costs
// ---------------------------------------------------------------------------
struct MarketOrderCostMetrics final {
  std::size_t market_orders_submitted{0};
  std::size_t market_orders_filled{0};
  std::size_t market_orders_unfilled{0};

  SampleStats effective_half_spread_ticks;
  SampleStats effective_half_spread_currency;
  SampleStats relative_effective_half_spread;
  SampleStats slippage_ticks;

  double total_effective_cost_currency{0.0};
  double total_fees_currency{0.0};
  double total_net_cost_currency{0.0};
};

// ---------------------------------------------------------------------------
// 14: Short-Horizon Post-Trade Price Movements (Adverse Selection)
// ---------------------------------------------------------------------------
struct HorizonAdverseSelection final {
  double horizon_seconds{0.0};
  std::size_t sample_size{0};
  std::size_t censored_or_missing_count{0};

  SampleStats spread_capture_currency;
  SampleStats immediate_execution_cost_currency;
  SampleStats price_drift_currency;
  SampleStats realized_spread_currency;

  double maker_fee_rebate_per_lot{0.0};
  SampleStats net_mtm_pnl_currency;
};

struct AdverseSelectionMetrics final {
  std::vector<HorizonAdverseSelection> horizons;
  std::string accounting_definitions;
};

// ---------------------------------------------------------------------------
// 15: Passive-Order Fill Statistics
// ---------------------------------------------------------------------------
struct PassiveFillMetrics final {
  std::size_t submitted_limit_orders{0};
  std::size_t fully_filled_orders{0};
  std::size_t partially_filled_orders{0};
  std::size_t cancelled_unfilled_orders{0};
  std::size_t cancelled_partially_filled_orders{0};
  std::size_t live_orders_remaining{0};

  core::QuantityLots total_submitted_lots{0};
  core::QuantityLots total_filled_lots{0};

  double order_fill_rate{0.0};
  double complete_fill_rate{0.0};
  double volume_fill_rate{0.0};

  SampleStats time_to_first_fill_s;
};

// ---------------------------------------------------------------------------
// Comprehensive Analytics Report
// ---------------------------------------------------------------------------
struct AnalyticsReport final {
  QuoteMetrics quotes;
  DepthMetrics depth;
  OrderFlowImbalanceMetrics ofi;
  TradeIntensityMetrics trade_intensity;
  SubmissionAndCancellationMetrics rates;
  OrderLifetimeMetrics order_lifetimes;
  TradeSizeMetrics trade_sizes;
  PriceReturnMetrics returns;
  RealizedVolatilityMetrics realized_volatility;
  MarketOrderCostMetrics market_costs;
  AdverseSelectionMetrics adverse_selection;
  PassiveFillMetrics passive_fills;

  [[nodiscard]] std::string to_formatted_report() const;
  [[nodiscard]] std::string to_json() const;
  [[nodiscard]] std::string to_ndjson() const;
};

}  // namespace lob::analytics
