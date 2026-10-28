#pragma once

#include "lob/core/types.hpp"
#include "lob/exchange/order_book.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lob::analytics {

/// Summary statistics for a sample of real-valued observations.
///
/// Uncertainty estimation:
/// In financial time-series and Hawkes-generated order flow, observations are
/// typically autocorrelated. Treating observations as i.i.d. underestimates the
/// standard error of the mean.
///
/// We provide:
///   se_iid: sample standard error under the i.i.d. assumption (s / sqrt(N))
///   se_hac: Newey-West Heteroskedasticity and Autocorrelation Consistent (HAC)
///           standard error of the sample mean with Bartlett kernel
///   autocorr_lag1: sample lag-1 autocorrelation coefficient rho_1
///   effective_sample_size: N * (1 - rho_1) / (1 + rho_1)
struct SampleStats final {
  std::size_t count{0};
  double mean{0.0};
  double std_dev{0.0};
  double min{0.0};
  double max{0.0};
  double p25{0.0};
  double median{0.0};
  double p75{0.0};
  double p95{0.0};
  double p99{0.0};
  double autocorr_lag1{0.0};
  double se_iid{0.0};
  double se_hac{0.0};
  double effective_sample_size{0.0};
};

/// Computes descriptive statistics and Newey-West HAC standard error for a sample.
///
/// @param values Real-valued observations in temporal order.
/// @param hac_max_lags Maximum lag truncation parameter for Bartlett kernel.
///                     If 0, defaults to min(20, floor(4 * (N/100)^(2/9))).
[[nodiscard]] SampleStats compute_sample_stats(
    const std::vector<double>& values, std::size_t hac_max_lags = 5);

/// Tracks the full deterministic lifecycle of an order from submission to termination.
struct OrderLifecycleRecord final {
  core::OrderId order_id{};
  core::Side side{core::Side::Buy};
  core::PriceTicks price_ticks{};
  core::QuantityLots initial_quantity{};
  core::QuantityLots cumulative_filled_quantity{};
  core::QuantityLots remaining_quantity{};
  core::TimestampNs submission_time_ns{};
  std::optional<core::TimestampNs> first_fill_time_ns{};
  std::optional<core::TimestampNs> terminal_time_ns{};
  exchange::OrderStatus terminal_status{exchange::OrderStatus::New};
};

/// Record of an individual passive fill and its multi-horizon price evaluation.
struct PassiveFillRecord final {
  core::TradeId trade_id{};
  core::TimestampNs trade_time_ns{};
  core::OrderId resting_order_id{};
  core::Side resting_side{core::Side::Buy};
  core::PriceTicks trade_price_ticks{};
  core::QuantityLots quantity{};
  std::optional<double> pre_trade_midprice_ticks{};
  /// Map of horizon (in seconds) to future midprice (in ticks).
  std::vector<std::pair<double, std::optional<double>>> future_midprices_ticks{};
};

}  // namespace lob::analytics
