#include "lob/analytics/microstructure_analytics.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <unordered_map>

namespace lob::analytics {
namespace {

[[nodiscard]] double ns_to_seconds(const core::TimestampNs ns) noexcept {
  return static_cast<double>(ns) / 1.0e9;
}

[[nodiscard]] core::TimestampNs seconds_to_ns(const double s) noexcept {
  return static_cast<core::TimestampNs>(std::floor(s * 1.0e9));
}

/// Find the latest snapshot with timestamp <= target_ns.
/// Snapshots must be in non-decreasing timestamp order.
[[nodiscard]] const exchange::BookSnapshot* find_snapshot_at_or_before(
    const std::vector<exchange::BookSnapshot>& snapshots,
    const core::TimestampNs target_ns) noexcept {
  if (snapshots.empty() || target_ns < snapshots.front().timestamp_ns) {
    return nullptr;
  }
  auto it = std::upper_bound(
      snapshots.begin(), snapshots.end(), target_ns,
      [](const core::TimestampNs val, const exchange::BookSnapshot& snap) {
        return val < snap.timestamp_ns;
      });
  if (it == snapshots.begin()) {
    return nullptr;
  }
  return &(*std::prev(it));
}

/// Extract midprice in ticks from a snapshot. Returns nullopt if either side is empty.
[[nodiscard]] std::optional<double> extract_midprice(
    const exchange::BookSnapshot& snap) noexcept {
  if (snap.bids.empty() || snap.asks.empty()) {
    return std::nullopt;
  }
  return (static_cast<double>(snap.bids.front().price_ticks) +
          static_cast<double>(snap.asks.front().price_ticks)) /
         2.0;
}

/// Extract microprice in ticks from a snapshot.
[[nodiscard]] std::optional<double> extract_microprice(
    const exchange::BookSnapshot& snap) noexcept {
  if (snap.bids.empty() || snap.asks.empty()) {
    return std::nullopt;
  }
  const double pb = static_cast<double>(snap.bids.front().price_ticks);
  const double pa = static_cast<double>(snap.asks.front().price_ticks);
  const double qb = static_cast<double>(snap.bids.front().quantity);
  const double qa = static_cast<double>(snap.asks.front().quantity);
  const double total_q = qb + qa;
  if (total_q <= 0.0) {
    return std::nullopt;
  }
  return (pb * qa + pa * qb) / total_q;
}

/// Compute depth for a side up to level K.
[[nodiscard]] core::QuantityLots sum_depth(
    const std::vector<exchange::LevelQuote>& levels,
    const std::size_t max_k) noexcept {
  core::QuantityLots total = 0;
  const std::size_t limit = std::min(max_k, levels.size());
  for (std::size_t i = 0; i < limit; ++i) {
    total += levels[i].quantity;
  }
  return total;
}

}  // namespace

AnalyticsReport MicrostructureAnalytics::compute(
    const AnalyticsInput& input, const AnalyticsConfig& config) {
  AnalyticsReport report;

  const auto& snapshots = input.snapshots;
  const auto& trades = input.trades;
  const auto& events = input.events;

  // Determine effective duration
  double duration = input.duration_seconds;
  if (duration <= 0.0) {
    core::TimestampNs max_ts = 0;
    if (!snapshots.empty()) {
      max_ts = std::max(max_ts, snapshots.back().timestamp_ns);
    }
    if (!trades.empty()) {
      max_ts = std::max(max_ts, trades.back().timestamp_ns);
    }
    duration = ns_to_seconds(max_ts);
    if (duration <= 0.0) {
      duration = 1.0;
    }
  }

  // -------------------------------------------------------------------------
  // 1, 2, 3: Quotes, Spreads, Midprice
  // -------------------------------------------------------------------------
  report.quotes.total_observations = snapshots.size();

  std::vector<double> best_bids;
  std::vector<double> best_asks;
  std::vector<double> spreads_ticks;
  std::vector<double> spreads_curr;
  std::vector<double> midprices_ticks;
  std::vector<double> midprices_curr;
  std::vector<double> microprices_ticks;

  double tw_spread_ticks_sum = 0.0;
  double tw_spread_curr_sum = 0.0;
  double tw_mid_ticks_sum = 0.0;
  double total_valid_quote_time_s = 0.0;

  for (std::size_t i = 0; i < snapshots.size(); ++i) {
    const auto& snap = snapshots[i];
    const bool has_bid = !snap.bids.empty();
    const bool has_ask = !snap.asks.empty();

    if (!has_bid && !has_ask) {
      ++report.quotes.empty_book_count;
      ++report.quotes.missing_bid_count;
      ++report.quotes.missing_ask_count;
      continue;
    }
    if (!has_bid) {
      ++report.quotes.missing_bid_count;
      best_asks.push_back(static_cast<double>(snap.asks.front().price_ticks));
      continue;
    }
    if (!has_ask) {
      ++report.quotes.missing_ask_count;
      best_bids.push_back(static_cast<double>(snap.bids.front().price_ticks));
      continue;
    }

    ++report.quotes.valid_two_sided_count;
    const double pb = static_cast<double>(snap.bids.front().price_ticks);
    const double pa = static_cast<double>(snap.asks.front().price_ticks);
    best_bids.push_back(pb);
    best_asks.push_back(pa);

    const double sp_ticks = pa - pb;
    const double sp_curr = sp_ticks * config.currency_per_tick;
    spreads_ticks.push_back(sp_ticks);
    spreads_curr.push_back(sp_curr);

    const double mid_ticks = (pb + pa) / 2.0;
    const double mid_curr = mid_ticks * config.currency_per_tick;
    midprices_ticks.push_back(mid_ticks);
    midprices_curr.push_back(mid_curr);

    const auto micro = extract_microprice(snap);
    if (micro) {
      microprices_ticks.push_back(*micro);
    }

    // Time-weighted calculations
    double dt_s = 0.0;
    if (i + 1 < snapshots.size()) {
      dt_s = ns_to_seconds(snapshots[i + 1].timestamp_ns - snap.timestamp_ns);
    } else {
      const double remaining_s = duration - ns_to_seconds(snap.timestamp_ns);
      dt_s = std::max(0.0, remaining_s);
    }
    if (dt_s > 0.0) {
      tw_spread_ticks_sum += sp_ticks * dt_s;
      tw_spread_curr_sum += sp_curr * dt_s;
      tw_mid_ticks_sum += mid_ticks * dt_s;
      total_valid_quote_time_s += dt_s;
    }
  }

  report.quotes.best_bid_ticks = compute_sample_stats(best_bids, config.hac_max_lags);
  report.quotes.best_ask_ticks = compute_sample_stats(best_asks, config.hac_max_lags);
  report.quotes.spread_ticks = compute_sample_stats(spreads_ticks, config.hac_max_lags);
  report.quotes.spread_currency = compute_sample_stats(spreads_curr, config.hac_max_lags);
  report.quotes.midprice_ticks = compute_sample_stats(midprices_ticks, config.hac_max_lags);
  report.quotes.midprice_currency = compute_sample_stats(midprices_curr, config.hac_max_lags);
  report.quotes.microprice_ticks = compute_sample_stats(microprices_ticks, config.hac_max_lags);

  if (total_valid_quote_time_s > 0.0) {
    report.quotes.time_weighted_spread_ticks = tw_spread_ticks_sum / total_valid_quote_time_s;
    report.quotes.time_weighted_spread_currency = tw_spread_curr_sum / total_valid_quote_time_s;
    report.quotes.time_weighted_midprice_ticks = tw_mid_ticks_sum / total_valid_quote_time_s;
  }

  // -------------------------------------------------------------------------
  // 4, 5: Displayed Depth & Order-Book Imbalance
  // -------------------------------------------------------------------------
  for (const std::size_t k : config.depth_levels) {
    LevelDepthStats lstats;
    lstats.level_k = k;

    std::vector<double> bid_depths;
    std::vector<double> ask_depths;
    std::vector<double> total_depths;
    std::vector<double> imbalances;

    for (const auto& snap : snapshots) {
      const auto qb = static_cast<double>(sum_depth(snap.bids, k));
      const auto qa = static_cast<double>(sum_depth(snap.asks, k));
      const double tot = qb + qa;

      bid_depths.push_back(qb);
      ask_depths.push_back(qa);
      total_depths.push_back(tot);

      if (tot <= 0.0) {
        ++lstats.zero_denominator_count;
      } else {
        ++lstats.valid_imbalance_count;
        imbalances.push_back((qb - qa) / tot);
      }
    }

    lstats.bid_depth_lots = compute_sample_stats(bid_depths, config.hac_max_lags);
    lstats.ask_depth_lots = compute_sample_stats(ask_depths, config.hac_max_lags);
    lstats.total_depth_lots = compute_sample_stats(total_depths, config.hac_max_lags);
    lstats.imbalance = compute_sample_stats(imbalances, config.hac_max_lags);

    report.depth.levels.push_back(lstats);
  }

  // -------------------------------------------------------------------------
  // 6: Order-Flow Imbalance (OFI)
  // -------------------------------------------------------------------------
  report.ofi.bin_size_seconds = 1.0;
  std::vector<double> event_ofis;
  std::map<std::int64_t, double> binned_ofi_map;

  if (snapshots.size() >= 2U) {
    report.ofi.total_event_transitions = snapshots.size() - 1U;

    for (std::size_t i = 1; i < snapshots.size(); ++i) {
      const auto& prev = snapshots[i - 1];
      const auto& curr = snapshots[i];

      const bool prev_has_b = !prev.bids.empty();
      const bool prev_has_a = !prev.asks.empty();
      const bool curr_has_b = !curr.bids.empty();
      const bool curr_has_a = !curr.asks.empty();

      double delta_of_b = 0.0;
      if (prev_has_b && curr_has_b) {
        const auto pb_prev = prev.bids.front().price_ticks;
        const auto pb_curr = curr.bids.front().price_ticks;
        const auto qb_prev = static_cast<double>(prev.bids.front().quantity);
        const auto qb_curr = static_cast<double>(curr.bids.front().quantity);

        if (pb_curr > pb_prev) {
          delta_of_b = qb_curr;
        } else if (pb_curr == pb_prev) {
          delta_of_b = qb_curr - qb_prev;
        } else {
          delta_of_b = -qb_prev;
        }
      } else if (!prev_has_b && curr_has_b) {
        delta_of_b = static_cast<double>(curr.bids.front().quantity);
      } else if (prev_has_b && !curr_has_b) {
        delta_of_b = -static_cast<double>(prev.bids.front().quantity);
      }

      double delta_of_a = 0.0;
      if (prev_has_a && curr_has_a) {
        const auto pa_prev = prev.asks.front().price_ticks;
        const auto pa_curr = curr.asks.front().price_ticks;
        const auto qa_prev = static_cast<double>(prev.asks.front().quantity);
        const auto qa_curr = static_cast<double>(curr.asks.front().quantity);

        if (pa_curr < pa_prev) {
          delta_of_a = qa_curr;
        } else if (pa_curr == pa_prev) {
          delta_of_a = qa_curr - qa_prev;
        } else {
          delta_of_a = -qa_prev;
        }
      } else if (!prev_has_a && curr_has_a) {
        delta_of_a = static_cast<double>(curr.asks.front().quantity);
      } else if (prev_has_a && !curr_has_a) {
        delta_of_a = -static_cast<double>(prev.asks.front().quantity);
      }

      const double net_ofi = delta_of_b - delta_of_a;
      event_ofis.push_back(net_ofi);
      report.ofi.cumulative_ofi_lots += static_cast<std::int64_t>(net_ofi);

      const double t_s = ns_to_seconds(curr.timestamp_ns);
      const auto bin_idx = static_cast<std::int64_t>(std::floor(t_s / report.ofi.bin_size_seconds));
      binned_ofi_map[bin_idx] += net_ofi;
    }
  }

  report.ofi.event_ofi_lots = compute_sample_stats(event_ofis, config.hac_max_lags);

  std::vector<double> binned_ofis;
  const auto total_bins = static_cast<std::int64_t>(std::ceil(duration / report.ofi.bin_size_seconds));
  for (std::int64_t b = 0; b < total_bins; ++b) {
    auto it = binned_ofi_map.find(b);
    binned_ofis.push_back(it != binned_ofi_map.end() ? it->second : 0.0);
  }
  report.ofi.bin_count = binned_ofis.size();
  report.ofi.binned_ofi_lots = compute_sample_stats(binned_ofis, config.hac_max_lags);

  // -------------------------------------------------------------------------
  // 7: Trade Arrival Intensity
  // -------------------------------------------------------------------------
  report.trade_intensity.duration_seconds = duration;
  report.trade_intensity.trade_count = trades.size();
  for (const auto& t : trades) {
    report.trade_intensity.total_volume_lots += t.quantity;
  }
  if (duration > 0.0) {
    report.trade_intensity.trade_arrival_rate_per_sec =
        static_cast<double>(trades.size()) / duration;
    report.trade_intensity.volume_arrival_rate_per_sec =
        static_cast<double>(report.trade_intensity.total_volume_lots) / duration;
  }

  // Dispersion ratio (Fano factor = Var(counts) / Mean(counts) in 1s bins)
  const double bin_sec = 1.0;
  const auto n_bins = static_cast<std::size_t>(std::max(1.0, std::ceil(duration / bin_sec)));
  std::vector<double> bin_counts(n_bins, 0.0);
  for (const auto& t : trades) {
    const double ts = ns_to_seconds(t.timestamp_ns);
    const auto b = static_cast<std::size_t>(std::min<double>(ts / bin_sec, static_cast<double>(n_bins - 1U)));
    bin_counts[b] += 1.0;
  }
  report.trade_intensity.dispersion_bin_count = n_bins;
  const auto bin_stats = compute_sample_stats(bin_counts, 0);
  if (bin_stats.mean > 1.0e-6) {
    report.trade_intensity.dispersion_ratio = (bin_stats.std_dev * bin_stats.std_dev) / bin_stats.mean;
  } else {
    report.trade_intensity.dispersion_ratio = 1.0;  // Degenerate Poisson reference
  }

  // -------------------------------------------------------------------------
  // 8: Order Submission & Cancellation Rates
  // -------------------------------------------------------------------------
  report.rates.duration_seconds = duration;
  std::size_t lb_count = 0;
  std::size_t la_count = 0;
  std::size_t mb_count = 0;
  std::size_t ms_count = 0;
  std::size_t cb_count = 0;
  std::size_t ca_count = 0;

  for (const auto& ev : events) {
    switch (ev.type) {
      case stochastic::EventType::LimitBuy: ++lb_count; break;
      case stochastic::EventType::LimitSell: ++la_count; break;
      case stochastic::EventType::MarketBuy: ++mb_count; break;
      case stochastic::EventType::MarketSell: ++ms_count; break;
      case stochastic::EventType::CancelBuy: ++cb_count; break;
      case stochastic::EventType::CancelSell: ++ca_count; break;
    }
  }

  report.rates.total_limit_submissions = lb_count + la_count;
  report.rates.total_cancellations = cb_count + ca_count;

  if (duration > 0.0) {
    report.rates.lb_rate_per_sec = static_cast<double>(lb_count) / duration;
    report.rates.la_rate_per_sec = static_cast<double>(la_count) / duration;
    report.rates.mb_rate_per_sec = static_cast<double>(mb_count) / duration;
    report.rates.ms_rate_per_sec = static_cast<double>(ms_count) / duration;
    report.rates.cb_rate_per_sec = static_cast<double>(cb_count) / duration;
    report.rates.ca_rate_per_sec = static_cast<double>(ca_count) / duration;

    report.rates.limit_submission_rate_per_sec =
        static_cast<double>(report.rates.total_limit_submissions) / duration;
    report.rates.market_submission_rate_per_sec =
        static_cast<double>(mb_count + ms_count) / duration;
    report.rates.cancellation_rate_per_sec =
        static_cast<double>(report.rates.total_cancellations) / duration;
  }

  if (report.rates.total_limit_submissions > 0) {
    report.rates.cancel_to_limit_ratio =
        static_cast<double>(report.rates.total_cancellations) /
        static_cast<double>(report.rates.total_limit_submissions);
    report.rates.cancel_to_limit_ratio_defined = true;
  }

  // -------------------------------------------------------------------------
  // 9, 15: Reconstruct Order Lifecycles & Passive Fill Statistics
  // -------------------------------------------------------------------------
  std::unordered_map<core::OrderId, OrderLifecycleRecord> order_records;

  // Record initial orders placed at t=0
  for (const auto& init_ord : input.initial_orders) {
    OrderLifecycleRecord rec;
    rec.order_id = init_ord.id;
    rec.side = init_ord.side;
    rec.price_ticks = init_ord.price_ticks;
    rec.initial_quantity = init_ord.quantity;
    rec.remaining_quantity = init_ord.quantity;
    rec.submission_time_ns = 0;
    rec.terminal_status = exchange::OrderStatus::Resting;
    order_records.emplace(init_ord.id, rec);
  }

  // Record limit submissions
  for (const auto& ev : events) {
    if (stochastic::is_limit_order(ev.type) &&
        ev.outcome == stochastic::ActionOutcome::Accepted) {
      OrderLifecycleRecord rec;
      rec.order_id = ev.order_id;
      rec.side = stochastic::is_buy_side(ev.type) ? core::Side::Buy : core::Side::Sell;
      rec.price_ticks = ev.price_ticks;
      rec.initial_quantity = ev.quantity;
      rec.remaining_quantity = ev.quantity;
      rec.submission_time_ns = seconds_to_ns(ev.time_seconds);
      rec.terminal_status = exchange::OrderStatus::Resting;
      order_records.emplace(ev.order_id, rec);
    }
  }

  // Process executions
  for (const auto& t : trades) {
    auto it = order_records.find(t.resting_order_id);
    if (it != order_records.end()) {
      auto& rec = it->second;
      if (!rec.first_fill_time_ns.has_value()) {
        rec.first_fill_time_ns = t.timestamp_ns;
      }
      rec.cumulative_filled_quantity += t.quantity;
      if (t.quantity >= rec.remaining_quantity) {
        rec.remaining_quantity = 0;
        rec.terminal_time_ns = t.timestamp_ns;
        rec.terminal_status = exchange::OrderStatus::Filled;
      } else {
        rec.remaining_quantity -= t.quantity;
        rec.terminal_status = exchange::OrderStatus::PartiallyFilled;
      }
    }
  }

  // Process cancellations
  for (const auto& ev : events) {
    if (stochastic::is_cancellation(ev.type) &&
        ev.outcome == stochastic::ActionOutcome::Accepted) {
      auto it = order_records.find(ev.order_id);
      if (it != order_records.end()) {
        auto& rec = it->second;
        if (rec.terminal_status != exchange::OrderStatus::Filled) {
          rec.terminal_time_ns = seconds_to_ns(ev.time_seconds);
          rec.terminal_status = exchange::OrderStatus::Cancelled;
        }
      }
    }
  }

  // Compute lifetime distributions and fill metrics
  std::vector<double> cancelled_lifetimes;
  std::vector<double> filled_lifetimes;
  std::vector<double> all_terminal_lifetimes;
  std::vector<double> time_to_first_fills;

  report.passive_fills.submitted_limit_orders = order_records.size();

  for (const auto& [oid, rec] : order_records) {
    report.passive_fills.total_submitted_lots += rec.initial_quantity;
    report.passive_fills.total_filled_lots += rec.cumulative_filled_quantity;

    if (rec.first_fill_time_ns.has_value()) {
      const double t_first_s = ns_to_seconds(*rec.first_fill_time_ns - rec.submission_time_ns);
      time_to_first_fills.push_back(std::max(0.0, t_first_s));
    }

    if (rec.terminal_status == exchange::OrderStatus::Filled) {
      ++report.passive_fills.fully_filled_orders;
      if (rec.terminal_time_ns.has_value()) {
        const double life_s = ns_to_seconds(*rec.terminal_time_ns - rec.submission_time_ns);
        filled_lifetimes.push_back(std::max(0.0, life_s));
        all_terminal_lifetimes.push_back(std::max(0.0, life_s));
      }
    } else if (rec.terminal_status == exchange::OrderStatus::Cancelled) {
      if (rec.cumulative_filled_quantity > 0) {
        ++report.passive_fills.cancelled_partially_filled_orders;
      } else {
        ++report.passive_fills.cancelled_unfilled_orders;
      }
      if (rec.terminal_time_ns.has_value()) {
        const double life_s = ns_to_seconds(*rec.terminal_time_ns - rec.submission_time_ns);
        cancelled_lifetimes.push_back(std::max(0.0, life_s));
        all_terminal_lifetimes.push_back(std::max(0.0, life_s));
      }
    } else {
      // Still live at simulation end (censored)
      ++report.order_lifetimes.live_orders_censored_count;
      ++report.passive_fills.live_orders_remaining;
      if (rec.cumulative_filled_quantity > 0) {
        ++report.passive_fills.partially_filled_orders;
      }
    }
  }

  report.order_lifetimes.cancelled_order_lifetimes_s =
      compute_sample_stats(cancelled_lifetimes, config.hac_max_lags);
  report.order_lifetimes.filled_order_lifetimes_s =
      compute_sample_stats(filled_lifetimes, config.hac_max_lags);
  report.order_lifetimes.all_terminal_order_lifetimes_s =
      compute_sample_stats(all_terminal_lifetimes, config.hac_max_lags);

  if (report.passive_fills.submitted_limit_orders > 0) {
    report.passive_fills.order_fill_rate =
        static_cast<double>(report.passive_fills.fully_filled_orders +
                            report.passive_fills.partially_filled_orders +
                            report.passive_fills.cancelled_partially_filled_orders) /
        static_cast<double>(report.passive_fills.submitted_limit_orders);
    report.passive_fills.complete_fill_rate =
        static_cast<double>(report.passive_fills.fully_filled_orders) /
        static_cast<double>(report.passive_fills.submitted_limit_orders);
  }
  if (report.passive_fills.total_submitted_lots > 0) {
    report.passive_fills.volume_fill_rate =
        static_cast<double>(report.passive_fills.total_filled_lots) /
        static_cast<double>(report.passive_fills.total_submitted_lots);
  }
  report.passive_fills.time_to_first_fill_s =
      compute_sample_stats(time_to_first_fills, config.hac_max_lags);

  // -------------------------------------------------------------------------
  // 10: Trade-Size Distributions
  // -------------------------------------------------------------------------
  std::vector<double> all_sizes;
  std::vector<double> buy_sizes;
  std::vector<double> sell_sizes;

  for (const auto& t : trades) {
    const auto sz = static_cast<double>(t.quantity);
    all_sizes.push_back(sz);
    if (t.aggressor_side == core::Side::Buy) {
      buy_sizes.push_back(sz);
      report.trade_sizes.buyer_volume_lots += t.quantity;
    } else {
      sell_sizes.push_back(sz);
      report.trade_sizes.seller_volume_lots += t.quantity;
    }
  }

  report.trade_sizes.overall_trade_sizes_lots =
      compute_sample_stats(all_sizes, config.hac_max_lags);
  report.trade_sizes.buyer_initiated_sizes_lots =
      compute_sample_stats(buy_sizes, config.hac_max_lags);
  report.trade_sizes.seller_initiated_sizes_lots =
      compute_sample_stats(sell_sizes, config.hac_max_lags);

  // -------------------------------------------------------------------------
  // 11: Price-Return Distributions at Configurable Horizons
  // -------------------------------------------------------------------------
  for (const double tau : config.return_horizons_seconds) {
    HorizonReturnStats hstats;
    hstats.horizon_seconds = tau;

    std::vector<double> ticks_rets;
    std::vector<double> simple_rets;
    std::vector<double> log_rets;

    const auto tau_ns = seconds_to_ns(tau);
    if (tau_ns > 0) {
      for (const auto& snap : snapshots) {
        const auto m0 = extract_midprice(snap);
        if (!m0.has_value() || *m0 <= 0.0) {
          ++hstats.missing_or_censored_count;
          continue;
        }

        const auto future_ts = snap.timestamp_ns + tau_ns;
        const auto* future_snap = find_snapshot_at_or_before(snapshots, future_ts);
        if (!future_snap || future_ts > seconds_to_ns(duration)) {
          // Beyond simulation duration -> right-censored
          ++hstats.missing_or_censored_count;
          continue;
        }

        const auto m1 = extract_midprice(*future_snap);
        if (!m1.has_value() || *m1 <= 0.0) {
          ++hstats.missing_or_censored_count;
          continue;
        }

        ++hstats.valid_observation_count;
        const double d_mid = *m1 - *m0;
        ticks_rets.push_back(d_mid);
        simple_rets.push_back(d_mid / *m0);
        log_rets.push_back(std::log(*m1 / *m0));
      }
    }

    hstats.tick_returns = compute_sample_stats(ticks_rets, config.hac_max_lags);
    hstats.simple_returns = compute_sample_stats(simple_rets, config.hac_max_lags);
    hstats.log_returns = compute_sample_stats(log_rets, config.hac_max_lags);

    // Skewness and Kurtosis for log returns
    if (log_rets.size() >= 3U && hstats.log_returns.std_dev > 1.0e-8) {
      const double m = hstats.log_returns.mean;
      const double s = hstats.log_returns.std_dev;
      const double n_d = static_cast<double>(log_rets.size());
      double sum_cube = 0.0;
      double sum_quad = 0.0;
      for (const double r : log_rets) {
        const double z = (r - m) / s;
        sum_cube += z * z * z;
        sum_quad += z * z * z * z;
      }
      hstats.skewness = sum_cube / n_d;
      hstats.kurtosis = (sum_quad / n_d) - 3.0;  // Excess kurtosis
    }

    report.returns.horizons.push_back(hstats);
  }

  // -------------------------------------------------------------------------
  // 12: Realized Volatility at Configurable Sampling Intervals
  // -------------------------------------------------------------------------
  for (const double delta : config.realized_vol_intervals_seconds) {
    IntervalRealizedVolatility rv_stat;
    rv_stat.interval_seconds = delta;

    if (delta <= 0.0 || duration < delta) {
      report.realized_volatility.intervals.push_back(rv_stat);
      continue;
    }

    const auto delta_ns = seconds_to_ns(delta);
    const auto total_steps = static_cast<std::size_t>(std::floor(duration / delta));
    rv_stat.interval_count = total_steps;

    double rv_sum = 0.0;
    std::optional<double> last_valid_mid;

    // Check t=0 midprice
    const auto* snap0 = find_snapshot_at_or_before(snapshots, 0);
    if (snap0) {
      last_valid_mid = extract_midprice(*snap0);
    }

    for (std::size_t k = 1; k <= total_steps; ++k) {
      const auto t_ns = static_cast<core::TimestampNs>(k) * delta_ns;
      const auto* snap_k = find_snapshot_at_or_before(snapshots, t_ns);

      std::optional<double> curr_mid;
      if (snap_k) {
        curr_mid = extract_midprice(*snap_k);
      }

      if (!curr_mid.has_value() || *curr_mid <= 0.0) {
        // Forward-fill if previous valid exists
        if (last_valid_mid.has_value()) {
          curr_mid = last_valid_mid;
          ++rv_stat.imputed_or_missing_intervals;
        } else {
          ++rv_stat.imputed_or_missing_intervals;
          continue;
        }
      }

      if (last_valid_mid.has_value() && *last_valid_mid > 0.0) {
        const double log_ret = std::log(*curr_mid / *last_valid_mid);
        rv_sum += log_ret * log_ret;
      }

      last_valid_mid = curr_mid;
    }

    rv_stat.realized_variance = rv_sum;
    rv_stat.realized_volatility = std::sqrt(rv_sum);
    if (duration > 0.0) {
      rv_stat.annualized_volatility =
          rv_stat.realized_volatility *
          std::sqrt(config.annualization_factor_seconds / duration);
    }

    report.realized_volatility.intervals.push_back(rv_stat);
  }

  // -------------------------------------------------------------------------
  // 13: Market-Order Execution Costs
  // -------------------------------------------------------------------------
  // Map trade incoming_order_id to trades
  std::unordered_map<core::OrderId, std::vector<exchange::Trade>> trades_by_incoming;
  for (const auto& t : trades) {
    trades_by_incoming[t.incoming_order_id].push_back(t);
  }

  std::vector<double> eff_half_ticks;
  std::vector<double> eff_half_curr;
  std::vector<double> rel_eff_spreads;
  std::vector<double> slippages_ticks;

  for (std::size_t i = 0; i < events.size(); ++i) {
    const auto& ev = events[i];
    if (!stochastic::is_market_order(ev.type)) {
      continue;
    }

    ++report.market_costs.market_orders_submitted;
    const auto tr_it = trades_by_incoming.find(ev.order_id);
    if (tr_it == trades_by_incoming.end() || tr_it->second.empty()) {
      ++report.market_costs.market_orders_unfilled;
      continue;
    }

    ++report.market_costs.market_orders_filled;
    const auto& m_trades = tr_it->second;

    const bool is_buy = stochastic::is_buy_side(ev.type);
    const double sign = is_buy ? 1.0 : -1.0;

    // Execution VWAP
    double volume_sum = 0.0;
    double cost_sum = 0.0;
    for (const auto& t : m_trades) {
      const auto q = static_cast<double>(t.quantity);
      volume_sum += q;
      cost_sum += static_cast<double>(t.price_ticks) * q;
    }
    const double vwap = (volume_sum > 0.0) ? (cost_sum / volume_sum) : 0.0;

    // Snapshot immediately preceding this event
    const auto ev_time_ns = seconds_to_ns(ev.time_seconds);
    // Find snapshot strictly before ev_time_ns, or at index i
    const exchange::BookSnapshot* pre_snap = nullptr;
    if (i < snapshots.size()) {
      pre_snap = &snapshots[i];  // snapshots[i] is recorded before event i or at t=0
    } else {
      pre_snap = find_snapshot_at_or_before(snapshots, ev_time_ns);
    }

    if (pre_snap) {
      const auto pre_mid = extract_midprice(*pre_snap);
      if (pre_mid.has_value()) {
        const double half_ticks = sign * (vwap - *pre_mid);
        const double half_curr = half_ticks * config.currency_per_tick;
        eff_half_ticks.push_back(half_ticks);
        eff_half_curr.push_back(half_curr);

        if (*pre_mid > 0.0) {
          rel_eff_spreads.push_back(half_ticks / *pre_mid);
        }

        const double order_cost = volume_sum * half_curr;
        const double order_fee = volume_sum * config.taker_fee_per_lot;
        report.market_costs.total_effective_cost_currency += order_cost;
        report.market_costs.total_fees_currency += order_fee;
        report.market_costs.total_net_cost_currency += (order_cost + order_fee);
      }

      // Pre-trade opposite quote slippage
      std::optional<core::PriceTicks> opp_quote;
      if (is_buy && !pre_snap->asks.empty()) {
        opp_quote = pre_snap->asks.front().price_ticks;
      } else if (!is_buy && !pre_snap->bids.empty()) {
        opp_quote = pre_snap->bids.front().price_ticks;
      }
      if (opp_quote.has_value()) {
        const double slip = sign * (vwap - static_cast<double>(*opp_quote));
        slippages_ticks.push_back(slip);
      }
    }
  }

  report.market_costs.effective_half_spread_ticks =
      compute_sample_stats(eff_half_ticks, config.hac_max_lags);
  report.market_costs.effective_half_spread_currency =
      compute_sample_stats(eff_half_curr, config.hac_max_lags);
  report.market_costs.relative_effective_half_spread =
      compute_sample_stats(rel_eff_spreads, config.hac_max_lags);
  report.market_costs.slippage_ticks =
      compute_sample_stats(slippages_ticks, config.hac_max_lags);

  // -------------------------------------------------------------------------
  // 14: Short-Horizon Post-Trade Price Movements (Adverse Selection)
  // -------------------------------------------------------------------------
  report.adverse_selection.accounting_definitions =
      "Adverse selection decomposition for passive resting fills:\n"
      "  SpreadCapture = S_passive * (M(t^-) - P_trade) * scale\n"
      "  PriceDrift(tau) = S_passive * (M(t^-) - M(t + tau)) * scale\n"
      "  RealizedSpread(tau) = S_passive * (M(t + tau) - P_trade) * scale\n"
      "  Identity: RealizedSpread(tau) = SpreadCapture - PriceDrift(tau)\n"
      "  NetPnL(tau) = RealizedSpread(tau) - maker_fee_rebate_per_lot";

  for (const double tau : config.adverse_selection_horizons_seconds) {
    HorizonAdverseSelection h_as;
    h_as.horizon_seconds = tau;
    h_as.maker_fee_rebate_per_lot = config.maker_fee_rebate_per_lot;

    std::vector<double> spread_captures;
    std::vector<double> exec_costs;
    std::vector<double> price_drifts;
    std::vector<double> realized_spreads;
    std::vector<double> net_pnls;

    const auto tau_ns = seconds_to_ns(tau);

    for (const auto& t : trades) {
      // Find snapshot immediately before trade timestamp
      const auto* pre_snap = find_snapshot_at_or_before(snapshots, t.timestamp_ns > 0 ? t.timestamp_ns - 1 : 0);
      if (!pre_snap) {
        pre_snap = find_snapshot_at_or_before(snapshots, t.timestamp_ns);
      }
      if (!pre_snap) {
        ++h_as.censored_or_missing_count;
        continue;
      }

      const auto m0 = extract_midprice(*pre_snap);
      if (!m0.has_value()) {
        ++h_as.censored_or_missing_count;
        continue;
      }

      const auto future_ts = t.timestamp_ns + tau_ns;
      if (future_ts > seconds_to_ns(duration)) {
        // Horizon beyond simulation run
        ++h_as.censored_or_missing_count;
        continue;
      }

      const auto* future_snap = find_snapshot_at_or_before(snapshots, future_ts);
      if (!future_snap) {
        ++h_as.censored_or_missing_count;
        continue;
      }

      const auto m_tau = extract_midprice(*future_snap);
      if (!m_tau.has_value()) {
        ++h_as.censored_or_missing_count;
        continue;
      }

      // Passive side sign: +1 for Buy, -1 for Sell
      const bool passive_is_buy = (t.aggressor_side == core::Side::Sell);
      const double s_pass = passive_is_buy ? 1.0 : -1.0;
      const double p_tr = static_cast<double>(t.price_ticks);

      // In currency units
      const double sc = s_pass * (*m0 - p_tr) * config.currency_per_tick;
      const double drift = s_pass * (*m0 - *m_tau) * config.currency_per_tick;
      const double r_spread = s_pass * (*m_tau - p_tr) * config.currency_per_tick;
      const double net_pnl = r_spread - config.maker_fee_rebate_per_lot;

      ++h_as.sample_size;
      spread_captures.push_back(sc);
      exec_costs.push_back(sc);
      price_drifts.push_back(drift);
      realized_spreads.push_back(r_spread);
      net_pnls.push_back(net_pnl);
    }

    h_as.spread_capture_currency = compute_sample_stats(spread_captures, config.hac_max_lags);
    h_as.immediate_execution_cost_currency = compute_sample_stats(exec_costs, config.hac_max_lags);
    h_as.price_drift_currency = compute_sample_stats(price_drifts, config.hac_max_lags);
    h_as.realized_spread_currency = compute_sample_stats(realized_spreads, config.hac_max_lags);
    h_as.net_mtm_pnl_currency = compute_sample_stats(net_pnls, config.hac_max_lags);

    report.adverse_selection.horizons.push_back(h_as);
  }

  return report;
}

AnalyticsReport MicrostructureAnalytics::compute(
    const stochastic::SyntheticMarketResult& result,
    const AnalyticsConfig& config) {
  AnalyticsInput input;
  input.snapshots = result.book_snapshots;
  input.trades = result.trades;
  input.events = result.events;
  input.initial_orders =
      stochastic::HawkesMarketAdapter::generate_initial_book(result.config.initial_book);
  input.duration_seconds = result.config.horizon_seconds;

  return compute(input, config);
}

// ---------------------------------------------------------------------------
// Human-readable formatted report
// ---------------------------------------------------------------------------
std::string AnalyticsReport::to_formatted_report() const {
  std::ostringstream out;
  out << std::fixed << std::setprecision(4);

  out << "================================================================================\n"
      << "                     MARKET MICROSTRUCTURE ANALYTICS REPORT                     \n"
      << "================================================================================\n\n";

  // Section 1-3: Quotes, Spreads, Midprice
  out << "1. QUOTES, SPREADS, AND MIDPRICE\n"
      << "--------------------------------------------------------------------------------\n"
      << "  Total Snapshots:          " << quotes.total_observations << "\n"
      << "  Valid Two-Sided Quotes:   " << quotes.valid_two_sided_count << "\n"
      << "  Missing Bid Quotes:       " << quotes.missing_bid_count << "\n"
      << "  Missing Ask Quotes:       " << quotes.missing_ask_count << "\n"
      << "  Empty Book Observations:  " << quotes.empty_book_count << "\n\n"
      << "  Metric                 Mean       StdDev     Median     SE(iid)    SE(HAC)   N_eff\n"
      << "  ------------------------------------------------------------------------------\n"
      << "  Best Bid (ticks)     " << std::setw(10) << quotes.best_bid_ticks.mean
      << std::setw(11) << quotes.best_bid_ticks.std_dev
      << std::setw(11) << quotes.best_bid_ticks.median
      << std::setw(11) << quotes.best_bid_ticks.se_iid
      << std::setw(11) << quotes.best_bid_ticks.se_hac
      << std::setw(8) << static_cast<std::size_t>(quotes.best_bid_ticks.effective_sample_size) << "\n"
      << "  Best Ask (ticks)     " << std::setw(10) << quotes.best_ask_ticks.mean
      << std::setw(11) << quotes.best_ask_ticks.std_dev
      << std::setw(11) << quotes.best_ask_ticks.median
      << std::setw(11) << quotes.best_ask_ticks.se_iid
      << std::setw(11) << quotes.best_ask_ticks.se_hac
      << std::setw(8) << static_cast<std::size_t>(quotes.best_ask_ticks.effective_sample_size) << "\n"
      << "  Spread (ticks)       " << std::setw(10) << quotes.spread_ticks.mean
      << std::setw(11) << quotes.spread_ticks.std_dev
      << std::setw(11) << quotes.spread_ticks.median
      << std::setw(11) << quotes.spread_ticks.se_iid
      << std::setw(11) << quotes.spread_ticks.se_hac
      << std::setw(8) << static_cast<std::size_t>(quotes.spread_ticks.effective_sample_size) << "\n"
      << "  Spread (currency)    " << std::setw(10) << quotes.spread_currency.mean
      << std::setw(11) << quotes.spread_currency.std_dev
      << std::setw(11) << quotes.spread_currency.median
      << std::setw(11) << quotes.spread_currency.se_iid
      << std::setw(11) << quotes.spread_currency.se_hac
      << std::setw(8) << static_cast<std::size_t>(quotes.spread_currency.effective_sample_size) << "\n"
      << "  Midprice (ticks)     " << std::setw(10) << quotes.midprice_ticks.mean
      << std::setw(11) << quotes.midprice_ticks.std_dev
      << std::setw(11) << quotes.midprice_ticks.median
      << std::setw(11) << quotes.midprice_ticks.se_iid
      << std::setw(11) << quotes.midprice_ticks.se_hac
      << std::setw(8) << static_cast<std::size_t>(quotes.midprice_ticks.effective_sample_size) << "\n"
      << "  Microprice (ticks)   " << std::setw(10) << quotes.microprice_ticks.mean
      << std::setw(11) << quotes.microprice_ticks.std_dev
      << std::setw(11) << quotes.microprice_ticks.median
      << std::setw(11) << quotes.microprice_ticks.se_iid
      << std::setw(11) << quotes.microprice_ticks.se_hac
      << std::setw(8) << static_cast<std::size_t>(quotes.microprice_ticks.effective_sample_size) << "\n\n"
      << "  Time-Weighted Spread:   " << quotes.time_weighted_spread_ticks << " ticks ("
      << quotes.time_weighted_spread_currency << " curr)\n"
      << "  Time-Weighted Midprice: " << quotes.time_weighted_midprice_ticks << " ticks\n\n";

  // Section 4-5: Depth & Order-Book Imbalance
  out << "2. DISPLAYED DEPTH AND ORDER-BOOK IMBALANCE (OBI)\n"
      << "--------------------------------------------------------------------------------\n";
  for (const auto& l : depth.levels) {
    out << "  [Depth Level K = " << l.level_k << "]\n"
        << "    Bid Depth (lots):    mean=" << l.bid_depth_lots.mean << " std=" << l.bid_depth_lots.std_dev
        << " med=" << l.bid_depth_lots.median << "\n"
        << "    Ask Depth (lots):    mean=" << l.ask_depth_lots.mean << " std=" << l.ask_depth_lots.std_dev
        << " med=" << l.ask_depth_lots.median << "\n"
        << "    Total Depth (lots):  mean=" << l.total_depth_lots.mean << " std=" << l.total_depth_lots.std_dev
        << " med=" << l.total_depth_lots.median << "\n"
        << "    Imbalance I_k:       mean=" << l.imbalance.mean << " std=" << l.imbalance.std_dev
        << " med=" << l.imbalance.median << " SE(HAC)=" << l.imbalance.se_hac << "\n"
        << "    Zero Denominators:   " << l.zero_denominator_count
        << " (Valid: " << l.valid_imbalance_count << ")\n";
  }
  out << "\n";

  // Section 6: Order-Flow Imbalance (OFI)
  out << "3. ORDER-FLOW IMBALANCE (OFI)\n"
      << "--------------------------------------------------------------------------------\n"
      << "  Cumulative OFI (lots):   " << ofi.cumulative_ofi_lots << "\n"
      << "  Event OFI per tick:      mean=" << ofi.event_ofi_lots.mean
      << " std=" << ofi.event_ofi_lots.std_dev << " SE(HAC)=" << ofi.event_ofi_lots.se_hac << "\n"
      << "  1s Binned OFI:           mean=" << ofi.binned_ofi_lots.mean
      << " std=" << ofi.binned_ofi_lots.std_dev << " bins=" << ofi.bin_count << "\n\n";

  // Section 7: Trade Arrival Intensity
  out << "4. TRADE ARRIVAL INTENSITY\n"
      << "--------------------------------------------------------------------------------\n"
      << "  Observation Duration:    " << trade_intensity.duration_seconds << " s\n"
      << "  Executed Trades:         " << trade_intensity.trade_count << "\n"
      << "  Total Volume:            " << trade_intensity.total_volume_lots << " lots\n"
      << "  Trade Arrival Rate:      " << trade_intensity.trade_arrival_rate_per_sec << " trades/s\n"
      << "  Volume Arrival Rate:     " << trade_intensity.volume_arrival_rate_per_sec << " lots/s\n"
      << "  Poisson Fano Factor:     " << trade_intensity.dispersion_ratio
      << " (D=1 homogeneous Poisson, D>1 clustered)\n\n";

  // Section 8: Submission and Cancellation Rates
  out << "5. SUBMISSION AND CANCELLATION RATES\n"
      << "--------------------------------------------------------------------------------\n"
      << "  Rates by Type (events/s):\n"
      << "    LB: " << rates.lb_rate_per_sec << " | LA: " << rates.la_rate_per_sec << "\n"
      << "    MB: " << rates.mb_rate_per_sec << " | MS: " << rates.ms_rate_per_sec << "\n"
      << "    CB: " << rates.cb_rate_per_sec << " | CA: " << rates.ca_rate_per_sec << "\n"
      << "  Aggregate Rates (events/s):\n"
      << "    Limit Submissions:     " << rates.limit_submission_rate_per_sec << " (N=" << rates.total_limit_submissions << ")\n"
      << "    Market Submissions:    " << rates.market_submission_rate_per_sec << "\n"
      << "    Cancellations:         " << rates.cancellation_rate_per_sec << " (N=" << rates.total_cancellations << ")\n"
      << "  Cancel-to-Limit Ratio:   " << rates.cancel_to_limit_ratio << "\n\n";

  // Section 9: Order Lifetime Distributions
  out << "6. ORDER LIFETIME DISTRIBUTIONS\n"
      << "--------------------------------------------------------------------------------\n"
      << "  Cancelled Lifetimes (s): mean=" << order_lifetimes.cancelled_order_lifetimes_s.mean
      << " med=" << order_lifetimes.cancelled_order_lifetimes_s.median
      << " p95=" << order_lifetimes.cancelled_order_lifetimes_s.p95
      << " N=" << order_lifetimes.cancelled_order_lifetimes_s.count << "\n"
      << "  Filled Lifetimes (s):    mean=" << order_lifetimes.filled_order_lifetimes_s.mean
      << " med=" << order_lifetimes.filled_order_lifetimes_s.median
      << " p95=" << order_lifetimes.filled_order_lifetimes_s.p95
      << " N=" << order_lifetimes.filled_order_lifetimes_s.count << "\n"
      << "  All Terminal Orders (s): mean=" << order_lifetimes.all_terminal_order_lifetimes_s.mean
      << " med=" << order_lifetimes.all_terminal_order_lifetimes_s.median
      << " N=" << order_lifetimes.all_terminal_order_lifetimes_s.count << "\n"
      << "  Live Orders (Censored):  " << order_lifetimes.live_orders_censored_count << "\n\n";

  // Section 10: Trade Size Distributions
  out << "7. TRADE SIZE DISTRIBUTIONS\n"
      << "--------------------------------------------------------------------------------\n"
      << "  Overall Trade Sizes:     mean=" << trade_sizes.overall_trade_sizes_lots.mean
      << " med=" << trade_sizes.overall_trade_sizes_lots.median
      << " p95=" << trade_sizes.overall_trade_sizes_lots.p95
      << " N=" << trade_sizes.overall_trade_sizes_lots.count << "\n"
      << "  Buyer-Initiated:         mean=" << trade_sizes.buyer_initiated_sizes_lots.mean
      << " vol=" << trade_sizes.buyer_volume_lots << " lots (N=" << trade_sizes.buyer_initiated_sizes_lots.count << ")\n"
      << "  Seller-Initiated:        mean=" << trade_sizes.seller_initiated_sizes_lots.mean
      << " vol=" << trade_sizes.seller_volume_lots << " lots (N=" << trade_sizes.seller_initiated_sizes_lots.count << ")\n\n";

  // Section 11: Price Return Distributions
  out << "8. PRICE RETURN DISTRIBUTIONS\n"
      << "--------------------------------------------------------------------------------\n";
  for (const auto& hr : returns.horizons) {
    out << "  [Horizon tau = " << hr.horizon_seconds << " s] (Valid N=" << hr.valid_observation_count
        << ", Censored/Missing=" << hr.missing_or_censored_count << ")\n"
        << "    Tick Return:   mean=" << hr.tick_returns.mean << " std=" << hr.tick_returns.std_dev
        << " min=" << hr.tick_returns.min << " max=" << hr.tick_returns.max << "\n"
        << "    Simple Return: mean=" << hr.simple_returns.mean << " std=" << hr.simple_returns.std_dev << "\n"
        << "    Log Return:    mean=" << hr.log_returns.mean << " std=" << hr.log_returns.std_dev
        << " skew=" << hr.skewness << " kurt=" << hr.kurtosis << "\n";
  }
  out << "\n";

  // Section 12: Realized Volatility
  out << "9. REALIZED VOLATILITY\n"
      << "--------------------------------------------------------------------------------\n";
  for (const auto& rv : realized_volatility.intervals) {
    out << "  [Interval delta = " << rv.interval_seconds << " s] (Intervals=" << rv.interval_count
        << ", Imputed=" << rv.imputed_or_missing_intervals << ")\n"
        << "    Realized Variance:     " << rv.realized_variance << "\n"
        << "    Realized Volatility:   " << rv.realized_volatility << "\n"
        << "    Annualized Volatility: " << rv.annualized_volatility << "\n";
  }
  out << "\n";

  // Section 13: Market-Order Execution Costs
  out << "10. MARKET-ORDER EXECUTION COSTS\n"
      << "--------------------------------------------------------------------------------\n"
      << "  Submitted Market Orders: " << market_costs.market_orders_submitted
      << " (Filled: " << market_costs.market_orders_filled
      << ", Unfilled: " << market_costs.market_orders_unfilled << ")\n"
      << "  Effective Half-Spread:   " << market_costs.effective_half_spread_ticks.mean << " ticks ("
      << market_costs.effective_half_spread_currency.mean << " curr)\n"
      << "  Relative Eff Half-Spread:" << market_costs.relative_effective_half_spread.mean << "\n"
      << "  Price Slippage:          " << market_costs.slippage_ticks.mean << " ticks\n"
      << "  Total Effective Cost:    " << market_costs.total_effective_cost_currency << " curr\n"
      << "  Total Taker Fees:        " << market_costs.total_fees_currency << " curr\n"
      << "  Total Net Cost:          " << market_costs.total_net_cost_currency << " curr\n\n";

  // Section 14: Adverse Selection
  out << "11. SHORT-HORIZON POST-TRADE PRICE MOVEMENTS (ADVERSE SELECTION)\n"
      << "--------------------------------------------------------------------------------\n";
  for (const auto& as : adverse_selection.horizons) {
    out << "  [Post-Trade Horizon tau = " << as.horizon_seconds << " s] (N=" << as.sample_size
        << ", Censored=" << as.censored_or_missing_count << ")\n"
        << "    Spread Capture:        mean=" << as.spread_capture_currency.mean
        << " SE(HAC)=" << as.spread_capture_currency.se_hac << "\n"
        << "    Subsequent Drift:      mean=" << as.price_drift_currency.mean
        << " SE(HAC)=" << as.price_drift_currency.se_hac << "\n"
        << "    Realized Spread:       mean=" << as.realized_spread_currency.mean
        << " SE(HAC)=" << as.realized_spread_currency.se_hac << "\n"
        << "    Maker Fee/Rebate:      " << as.maker_fee_rebate_per_lot << " /lot\n"
        << "    Net MTM Performance:   mean=" << as.net_mtm_pnl_currency.mean
        << " SE(HAC)=" << as.net_mtm_pnl_currency.se_hac << "\n";
  }
  out << "\n";

  // Section 15: Passive Fill Statistics
  out << "12. PASSIVE ORDER FILL STATISTICS\n"
      << "--------------------------------------------------------------------------------\n"
      << "  Submitted Limit Orders:  " << passive_fills.submitted_limit_orders << "\n"
      << "  Fully Filled:            " << passive_fills.fully_filled_orders << "\n"
      << "  Partially Filled Live:   " << passive_fills.partially_filled_orders << "\n"
      << "  Cancelled Unfilled:      " << passive_fills.cancelled_unfilled_orders << "\n"
      << "  Cancelled Partial Fill:  " << passive_fills.cancelled_partially_filled_orders << "\n"
      << "  Live Orders Remaining:   " << passive_fills.live_orders_remaining << "\n"
      << "  Order Fill Rate:         " << (passive_fills.order_fill_rate * 100.0) << " %\n"
      << "  Complete Fill Rate:      " << (passive_fills.complete_fill_rate * 100.0) << " %\n"
      << "  Volume Fill Rate:        " << (passive_fills.volume_fill_rate * 100.0) << " % ("
      << passive_fills.total_filled_lots << " / " << passive_fills.total_submitted_lots << " lots)\n"
      << "  Time to First Fill (s):  mean=" << passive_fills.time_to_first_fill_s.mean
      << " med=" << passive_fills.time_to_first_fill_s.median
      << " N=" << passive_fills.time_to_first_fill_s.count << "\n\n"
      << "================================================================================\n";

  return out.str();
}

// ---------------------------------------------------------------------------
// JSON serialization
// ---------------------------------------------------------------------------
namespace {
std::string stats_to_json(const SampleStats& s) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6);
  out << "{\"count\":" << s.count
      << ",\"mean\":" << s.mean
      << ",\"std_dev\":" << s.std_dev
      << ",\"min\":" << s.min
      << ",\"max\":" << s.max
      << ",\"median\":" << s.median
      << ",\"p25\":" << s.p25
      << ",\"p75\":" << s.p75
      << ",\"p95\":" << s.p95
      << ",\"p99\":" << s.p99
      << ",\"autocorr_lag1\":" << s.autocorr_lag1
      << ",\"se_iid\":" << s.se_iid
      << ",\"se_hac\":" << s.se_hac
      << ",\"effective_sample_size\":" << s.effective_sample_size
      << "}";
  return out.str();
}
}  // namespace

std::string AnalyticsReport::to_json() const {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6);
  out << "{\n"
      << "  \"quotes\": {\n"
      << "    \"total_observations\": " << quotes.total_observations << ",\n"
      << "    \"valid_two_sided_count\": " << quotes.valid_two_sided_count << ",\n"
      << "    \"spread_ticks\": " << stats_to_json(quotes.spread_ticks) << ",\n"
      << "    \"spread_currency\": " << stats_to_json(quotes.spread_currency) << ",\n"
      << "    \"time_weighted_spread_ticks\": " << quotes.time_weighted_spread_ticks << ",\n"
      << "    \"midprice_ticks\": " << stats_to_json(quotes.midprice_ticks) << ",\n"
      << "    \"microprice_ticks\": " << stats_to_json(quotes.microprice_ticks) << "\n"
      << "  },\n"
      << "  \"order_flow_imbalance\": {\n"
      << "    \"cumulative_ofi_lots\": " << ofi.cumulative_ofi_lots << ",\n"
      << "    \"event_ofi\": " << stats_to_json(ofi.event_ofi_lots) << ",\n"
      << "    \"binned_ofi\": " << stats_to_json(ofi.binned_ofi_lots) << "\n"
      << "  },\n"
      << "  \"trade_intensity\": {\n"
      << "    \"duration_seconds\": " << trade_intensity.duration_seconds << ",\n"
      << "    \"trade_count\": " << trade_intensity.trade_count << ",\n"
      << "    \"total_volume_lots\": " << trade_intensity.total_volume_lots << ",\n"
      << "    \"trade_arrival_rate\": " << trade_intensity.trade_arrival_rate_per_sec << ",\n"
      << "    \"dispersion_ratio\": " << trade_intensity.dispersion_ratio << "\n"
      << "  },\n"
      << "  \"rates\": {\n"
      << "    \"limit_submission_rate\": " << rates.limit_submission_rate_per_sec << ",\n"
      << "    \"market_submission_rate\": " << rates.market_submission_rate_per_sec << ",\n"
      << "    \"cancellation_rate\": " << rates.cancellation_rate_per_sec << ",\n"
      << "    \"cancel_to_limit_ratio\": " << rates.cancel_to_limit_ratio << "\n"
      << "  },\n"
      << "  \"market_costs\": {\n"
      << "    \"submitted\": " << market_costs.market_orders_submitted << ",\n"
      << "    \"filled\": " << market_costs.market_orders_filled << ",\n"
      << "    \"effective_half_spread_ticks\": " << stats_to_json(market_costs.effective_half_spread_ticks) << ",\n"
      << "    \"total_net_cost\": " << market_costs.total_net_cost_currency << "\n"
      << "  },\n"
      << "  \"passive_fills\": {\n"
      << "    \"submitted\": " << passive_fills.submitted_limit_orders << ",\n"
      << "    \"fully_filled\": " << passive_fills.fully_filled_orders << ",\n"
      << "    \"order_fill_rate\": " << passive_fills.order_fill_rate << ",\n"
      << "    \"volume_fill_rate\": " << passive_fills.volume_fill_rate << "\n"
      << "  }\n"
      << "}\n";
  return out.str();
}

std::string AnalyticsReport::to_ndjson() const {
  std::ostringstream out;
  out << "{\"category\":\"quotes\",\"valid_two_sided\":" << quotes.valid_two_sided_count
      << ",\"mean_spread\":" << quotes.spread_ticks.mean
      << ",\"mean_mid\":" << quotes.midprice_ticks.mean << "}\n"
      << "{\"category\":\"ofi\",\"cumulative_lots\":" << ofi.cumulative_ofi_lots
      << ",\"mean_event_ofi\":" << ofi.event_ofi_lots.mean << "}\n"
      << "{\"category\":\"trades\",\"count\":" << trade_intensity.trade_count
      << ",\"rate\":" << trade_intensity.trade_arrival_rate_per_sec
      << ",\"fano\":" << trade_intensity.dispersion_ratio << "}\n"
      << "{\"category\":\"rates\",\"limit_rate\":" << rates.limit_submission_rate_per_sec
      << ",\"cancel_rate\":" << rates.cancellation_rate_per_sec
      << ",\"csr\":" << rates.cancel_to_limit_ratio << "}\n"
      << "{\"category\":\"costs\",\"filled_market_orders\":" << market_costs.market_orders_filled
      << ",\"total_net_cost\":" << market_costs.total_net_cost_currency << "}\n"
      << "{\"category\":\"passive_fills\",\"order_fill_rate\":" << passive_fills.order_fill_rate
      << ",\"volume_fill_rate\":" << passive_fills.volume_fill_rate << "}\n";
  return out.str();
}

}  // namespace lob::analytics
