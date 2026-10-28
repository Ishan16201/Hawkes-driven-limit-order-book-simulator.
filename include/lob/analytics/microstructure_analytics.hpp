#pragma once

#include "lob/analytics/metrics.hpp"
#include "lob/analytics/types.hpp"
#include "lob/exchange/matching_engine.hpp"
#include "lob/stochastic/hawkes_market_adapter.hpp"

#include <vector>

namespace lob::analytics {

struct AnalyticsConfig final {
  /// Conversion scale: currency units per price tick.
  double currency_per_tick{1.0};

  /// Cash flow per lot executed as passive maker (negative indicates rebate, positive indicates fee).
  double maker_fee_rebate_per_lot{0.0};

  /// Cash flow per lot executed as aggressive taker (positive indicates fee).
  double taker_fee_per_lot{0.0};

  /// Price-level depths to analyze for displayed depth and order-book imbalance.
  std::vector<std::size_t> depth_levels{1, 5};

  /// Evaluation horizons (in seconds) for price return distributions.
  std::vector<double> return_horizons_seconds{0.1, 0.5, 1.0, 5.0};

  /// Sampling intervals (in seconds) for realized volatility.
  std::vector<double> realized_vol_intervals_seconds{0.1, 1.0};

  /// Evaluation horizons (in seconds) for adverse selection / post-trade drift.
  std::vector<double> adverse_selection_horizons_seconds{0.05, 0.1, 0.5, 1.0};

  /// Time scaling factor for annualizing realized volatility (default: 252 days * 6.5 hours/day).
  double annualization_factor_seconds{252.0 * 6.5 * 3600.0};

  /// Maximum lags for Newey-West HAC standard error estimation.
  std::size_t hac_max_lags{5};
};

/// Complete input dataset for microstructure analytics.
/// Can be constructed from a simulation run or from replay of canonical events.
struct AnalyticsInput final {
  /// Sequence of book snapshots in non-decreasing timestamp order.
  std::vector<exchange::BookSnapshot> snapshots;

  /// Executed trades during the period.
  std::vector<exchange::Trade> trades;

  /// Market event records (submissions, cancellations, rejections).
  std::vector<stochastic::MarketEventRecord> events;

  /// Initial resting orders placed at t=0 (if any).
  std::vector<stochastic::InitialBookOrder> initial_orders;

  /// Overall duration in seconds. If 0, derived from snapshots or events.
  double duration_seconds{0.0};
};

class MicrostructureAnalytics final {
 public:
  /// Computes all 15 microstructure analytics metrics from the provided input.
  [[nodiscard]] static AnalyticsReport compute(
      const AnalyticsInput& input, const AnalyticsConfig& config = {});

  /// Convenience method to compute analytics directly from a SyntheticMarketResult.
  [[nodiscard]] static AnalyticsReport compute(
      const stochastic::SyntheticMarketResult& result,
      const AnalyticsConfig& config = {});
};

}  // namespace lob::analytics
