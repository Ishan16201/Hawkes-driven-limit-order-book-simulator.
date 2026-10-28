#include "lob/analytics/microstructure_analytics.hpp"
#include "lob/analytics/types.hpp"
#include "lob/stochastic/hawkes_market_adapter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace lob;
using namespace lob::analytics;
using Catch::Matchers::WithinAbs;

TEST_CASE("SampleStats handles empty, single-element, and hand-calculated vectors", "[analytics]") {
  SECTION("empty vector") {
    const auto stats = compute_sample_stats({});
    REQUIRE(stats.count == 0);
    REQUIRE(stats.mean == 0.0);
    REQUIRE(stats.std_dev == 0.0);
    REQUIRE(stats.se_iid == 0.0);
    REQUIRE(stats.se_hac == 0.0);
  }

  SECTION("single element") {
    const auto stats = compute_sample_stats({42.0});
    REQUIRE(stats.count == 1);
    REQUIRE(stats.mean == 42.0);
    REQUIRE(stats.min == 42.0);
    REQUIRE(stats.max == 42.0);
    REQUIRE(stats.median == 42.0);
    REQUIRE(stats.std_dev == 0.0);
    REQUIRE(stats.se_iid == 0.0);
    REQUIRE(stats.se_hac == 0.0);
  }

  SECTION("hand-calculated five-element vector {1, 2, 3, 4, 5}") {
    const std::vector<double> vals = {1.0, 2.0, 3.0, 4.0, 5.0};
    const auto stats = compute_sample_stats(vals);
    REQUIRE(stats.count == 5);
    REQUIRE_THAT(stats.mean, WithinAbs(3.0, 1e-9));
    // Sample variance s^2 = (4 + 1 + 0 + 1 + 4) / 4 = 10 / 4 = 2.5
    // std_dev = sqrt(2.5) ~ 1.58113883
    REQUIRE_THAT(stats.std_dev, WithinAbs(std::sqrt(2.5), 1e-6));
    REQUIRE_THAT(stats.min, WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(stats.max, WithinAbs(5.0, 1e-9));
    REQUIRE_THAT(stats.median, WithinAbs(3.0, 1e-9));
    REQUIRE_THAT(stats.p25, WithinAbs(2.0, 1e-9));
    REQUIRE_THAT(stats.p75, WithinAbs(4.0, 1e-9));
    REQUIRE_THAT(stats.se_iid, WithinAbs(std::sqrt(2.5 / 5.0), 1e-6));
  }

  SECTION("autocorrelated sequence has SE(HAC) > SE(iid)") {
    // Strongly positively autocorrelated sequence: [1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6]
    std::vector<double> correlated;
    for (int i = 0; i < 20; ++i) {
      correlated.push_back(static_cast<double>(i / 2));
    }
    const auto stats = compute_sample_stats(correlated, 4);
    REQUIRE(stats.autocorr_lag1 > 0.5);
    // Under positive autocorrelation, HAC standard error exceeds naive i.i.d. standard error
    REQUIRE(stats.se_hac > stats.se_iid);
  }
}

TEST_CASE("Hand-calculated quotes, spreads, midprice, and time-weighting", "[analytics]") {
  AnalyticsInput input;
  input.duration_seconds = 10.0;

  // Snapshot 0 at t=0s: bid 100 @ 10, ask 104 @ 10 (spread=4, mid=102)
  exchange::BookSnapshot s0;
  s0.timestamp_ns = 0;
  s0.bids.push_back({.price_ticks = 100, .quantity = 10});
  s0.asks.push_back({.price_ticks = 104, .quantity = 10});
  input.snapshots.push_back(s0);

  // Snapshot 1 at t=4s: bid 101 @ 10, ask 103 @ 10 (spread=2, mid=102) -> active for 6s (from 4s to 10s)
  exchange::BookSnapshot s1;
  s1.timestamp_ns = 4'000'000'000;
  s1.bids.push_back({.price_ticks = 101, .quantity = 10});
  s1.asks.push_back({.price_ticks = 103, .quantity = 10});
  input.snapshots.push_back(s1);

  AnalyticsConfig config;
  config.currency_per_tick = 0.50;  // 50 cents per tick

  const auto report = MicrostructureAnalytics::compute(input, config);

  REQUIRE(report.quotes.total_observations == 2);
  REQUIRE(report.quotes.valid_two_sided_count == 2);
  REQUIRE_THAT(report.quotes.best_bid_ticks.mean, WithinAbs(100.5, 1e-9));
  REQUIRE_THAT(report.quotes.best_ask_ticks.mean, WithinAbs(103.5, 1e-9));

  // Spreads: 4 ticks and 2 ticks -> mean = 3 ticks
  REQUIRE_THAT(report.quotes.spread_ticks.mean, WithinAbs(3.0, 1e-9));
  // In currency: 4 * 0.5 = 2.0, 2 * 0.5 = 1.0 -> mean = 1.5
  REQUIRE_THAT(report.quotes.spread_currency.mean, WithinAbs(1.5, 1e-9));

  // Time-weighted spread:
  // s0 active for 4s (spread=4)
  // s1 active for 6s (spread=2)
  // tw_spread = (4*4 + 2*6) / 10 = (16 + 12) / 10 = 2.8 ticks
  REQUIRE_THAT(report.quotes.time_weighted_spread_ticks, WithinAbs(2.8, 1e-6));
  REQUIRE_THAT(report.quotes.time_weighted_spread_currency, WithinAbs(1.4, 1e-6));

  // Midprice: (102 + 102)/2 = 102 ticks
  REQUIRE_THAT(report.quotes.midprice_ticks.mean, WithinAbs(102.0, 1e-9));
}

TEST_CASE("Hand-calculated displayed depth and order-book imbalance (OBI)", "[analytics]") {
  AnalyticsInput input;
  input.duration_seconds = 5.0;

  // Snapshot:
  // Bids: 100 @ 10, 99 @ 20
  // Asks: 102 @ 15, 103 @ 25
  exchange::BookSnapshot s;
  s.timestamp_ns = 0;
  s.bids.push_back({.price_ticks = 100, .quantity = 10});
  s.bids.push_back({.price_ticks = 99, .quantity = 20});
  s.asks.push_back({.price_ticks = 102, .quantity = 15});
  s.asks.push_back({.price_ticks = 103, .quantity = 25});
  input.snapshots.push_back(s);

  AnalyticsConfig config;
  config.depth_levels = {1, 2};

  const auto report = MicrostructureAnalytics::compute(input, config);
  REQUIRE(report.depth.levels.size() == 2);

  // Level 1: Bid=10, Ask=15, Total=25. I_1 = (10 - 15) / 25 = -0.2
  const auto& l1 = report.depth.levels[0];
  REQUIRE(l1.level_k == 1);
  REQUIRE_THAT(l1.bid_depth_lots.mean, WithinAbs(10.0, 1e-9));
  REQUIRE_THAT(l1.ask_depth_lots.mean, WithinAbs(15.0, 1e-9));
  REQUIRE_THAT(l1.total_depth_lots.mean, WithinAbs(25.0, 1e-9));
  REQUIRE_THAT(l1.imbalance.mean, WithinAbs(-0.2, 1e-9));
  REQUIRE(l1.zero_denominator_count == 0);

  // Level 2: Bid=10+20=30, Ask=15+25=40, Total=70. I_2 = (30 - 40) / 70 = -10 / 70 = -0.14285714
  const auto& l2 = report.depth.levels[1];
  REQUIRE(l2.level_k == 2);
  REQUIRE_THAT(l2.bid_depth_lots.mean, WithinAbs(30.0, 1e-9));
  REQUIRE_THAT(l2.ask_depth_lots.mean, WithinAbs(40.0, 1e-9));
  REQUIRE_THAT(l2.total_depth_lots.mean, WithinAbs(70.0, 1e-9));
  REQUIRE_THAT(l2.imbalance.mean, WithinAbs(-10.0 / 70.0, 1e-6));
}

TEST_CASE("Hand-calculated Order-Flow Imbalance (OFI)", "[analytics]") {
  AnalyticsInput input;
  input.duration_seconds = 4.0;

  // Step 0 (t=0): bid 100 @ 10, ask 102 @ 10
  exchange::BookSnapshot s0;
  s0.timestamp_ns = 0;
  s0.bids.push_back({100, 10});
  s0.asks.push_back({102, 10});
  input.snapshots.push_back(s0);

  // Step 1 (t=1): bid increases to 101 @ 8, ask stays 102 @ 10 -> delta_b = +8, delta_a = 0 -> OFI = +8
  exchange::BookSnapshot s1;
  s1.timestamp_ns = 1'000'000'000;
  s1.bids.push_back({101, 8});
  s1.asks.push_back({102, 10});
  input.snapshots.push_back(s1);

  // Step 2 (t=2): bid stays 101, qty increases to 12 -> delta_b = 12 - 8 = +4 -> OFI = +4
  exchange::BookSnapshot s2;
  s2.timestamp_ns = 2'000'000'000;
  s2.bids.push_back({101, 12});
  s2.asks.push_back({102, 10});
  input.snapshots.push_back(s2);

  // Step 3 (t=3): ask price drops to 101 @ 5 -> delta_a = +5 -> OFI = 0 - 5 = -5
  exchange::BookSnapshot s3;
  s3.timestamp_ns = 3'000'000'000;
  s3.bids.push_back({101, 12});
  s3.asks.push_back({101, 5});
  input.snapshots.push_back(s3);

  const auto report = MicrostructureAnalytics::compute(input);

  REQUIRE(report.ofi.total_event_transitions == 3);
  // Cumulative OFI = +8 + 4 - 5 = 7
  REQUIRE(report.ofi.cumulative_ofi_lots == 7);
  REQUIRE_THAT(report.ofi.event_ofi_lots.mean, WithinAbs(7.0 / 3.0, 1e-6));
}

TEST_CASE("Empty book, irregular timestamps, and zero denominators are handled gracefully", "[analytics]") {
  AnalyticsInput input;
  input.duration_seconds = 10.0;

  // Snapshot with empty book at t=1.2345s
  exchange::BookSnapshot s0;
  s0.timestamp_ns = 1'234'567'890;
  input.snapshots.push_back(s0);

  // Snapshot with bid only at t=5.6789s
  exchange::BookSnapshot s1;
  s1.timestamp_ns = 5'678'901'234;
  s1.bids.push_back({100, 10});
  input.snapshots.push_back(s1);

  // Snapshot with ask only at t=8.0s
  exchange::BookSnapshot s2;
  s2.timestamp_ns = 8'000'000'000;
  s2.asks.push_back({105, 10});
  input.snapshots.push_back(s2);

  const auto report = MicrostructureAnalytics::compute(input);

  REQUIRE(report.quotes.total_observations == 3);
  REQUIRE(report.quotes.valid_two_sided_count == 0);
  REQUIRE(report.quotes.empty_book_count == 1);
  REQUIRE(report.quotes.missing_bid_count == 2);
  REQUIRE(report.quotes.missing_ask_count == 2);

  // Depth imbalance zero denominator recorded for snapshot 0
  REQUIRE(report.depth.levels[0].zero_denominator_count >= 1);
}

TEST_CASE("No-trade period handles intensity, costs, and fill distributions without error", "[analytics]") {
  AnalyticsInput input;
  input.duration_seconds = 5.0;

  // Only quote snapshots, no trades
  exchange::BookSnapshot s;
  s.timestamp_ns = 0;
  s.bids.push_back({100, 10});
  s.asks.push_back({102, 10});
  input.snapshots.push_back(s);

  const auto report = MicrostructureAnalytics::compute(input);

  REQUIRE(report.trade_intensity.trade_count == 0);
  REQUIRE(report.trade_intensity.trade_arrival_rate_per_sec == 0.0);
  REQUIRE(report.trade_intensity.total_volume_lots == 0);
  REQUIRE(report.market_costs.market_orders_submitted == 0);
  REQUIRE(report.adverse_selection.horizons.front().sample_size == 0);
}

TEST_CASE("Adverse selection decomposition satisfies accounting identity", "[analytics]") {
  AnalyticsInput input;
  input.duration_seconds = 5.0;

  // Snapshots:
  // t=0.0s: bid 100 @ 10, ask 104 @ 10 -> Mid = 102
  exchange::BookSnapshot s0;
  s0.timestamp_ns = 0;
  s0.bids.push_back({100, 10});
  s0.asks.push_back({104, 10});
  input.snapshots.push_back(s0);

  // At t=1.0s: Trade executes against resting buy order at price 100 (aggressor = Sell).
  // Immediately after trade, midprice shifts to 101 (bid was filled/depleted).
  exchange::Trade trade;
  trade.id = 1;
  trade.timestamp_ns = 1'000'000'000;
  trade.price_ticks = 100;
  trade.quantity = 5;
  trade.aggressor_side = core::Side::Sell;  // Resting order is Buy!
  trade.resting_order_id = 10;
  trade.incoming_order_id = 20;
  input.trades.push_back(trade);

  exchange::BookSnapshot s1;
  s1.timestamp_ns = 1'000'000'000;
  s1.bids.push_back({98, 10});
  s1.asks.push_back({104, 10});  // Mid = (98 + 104)/2 = 101
  input.snapshots.push_back(s1);

  // At t=2.0s: Future midprice at horizon tau=1.0s is 101.
  exchange::BookSnapshot s2;
  s2.timestamp_ns = 2'000'000'000;
  s2.bids.push_back({98, 10});
  s2.asks.push_back({104, 10});
  input.snapshots.push_back(s2);

  AnalyticsConfig config;
  config.currency_per_tick = 1.0;
  config.adverse_selection_horizons_seconds = {1.0};
  config.maker_fee_rebate_per_lot = -0.05;  // 5 cents rebate per lot

  const auto report = MicrostructureAnalytics::compute(input, config);

  const auto& as = report.adverse_selection.horizons.front();
  REQUIRE(as.sample_size == 1);

  // Pre-trade midprice M(t^-) = 102.
  // Trade price P = 100. Resting side = Buy (S_passive = +1).
  // Spread capture = S_passive * (M(t^-) - P) = +1 * (102 - 100) = +2.0
  REQUIRE_THAT(as.spread_capture_currency.mean, WithinAbs(2.0, 1e-6));

  // Future midprice M(t + 1.0s) = 101.
  // Subsequent price drift = S_passive * (M(t^-) - M(t + tau)) = +1 * (102 - 101) = +1.0 (price fell against maker)
  REQUIRE_THAT(as.price_drift_currency.mean, WithinAbs(1.0, 1e-6));

  // Realized spread = S_passive * (M(t + tau) - P) = +1 * (101 - 100) = +1.0
  REQUIRE_THAT(as.realized_spread_currency.mean, WithinAbs(1.0, 1e-6));

  // Fundamental Identity: RealizedSpread == SpreadCapture - PriceDrift
  REQUIRE_THAT(as.realized_spread_currency.mean,
               WithinAbs(as.spread_capture_currency.mean - as.price_drift_currency.mean, 1e-9));

  // Net MTM PnL = RealizedSpread - maker_fee = 1.0 - (-0.05) = 1.05
  REQUIRE_THAT(as.net_mtm_pnl_currency.mean, WithinAbs(1.05, 1e-6));
}

TEST_CASE("End-to-end integration: synthetic Hawkes simulation produces reproducible analytics", "[analytics]") {
  stochastic::SyntheticMarketConfig config;
  config.hawkes.baseline_events_per_second = {1.0, 1.0, 0.5, 0.5, 0.3, 0.3};
  config.hawkes.alpha_branching = {
      {0.1, 0.0, 0.0, 0.0, 0.1, 0.0},
      {0.0, 0.1, 0.0, 0.0, 0.0, 0.1},
      {0.1, 0.0, 0.1, 0.0, 0.0, 0.0},
      {0.0, 0.1, 0.0, 0.1, 0.0, 0.0},
      {0.05, 0.0, 0.0, 0.0, 0.1, 0.0},
      {0.0, 0.05, 0.0, 0.0, 0.0, 0.1},
  };
  config.hawkes.beta_per_second = {
      {5.0, 5.0, 5.0, 5.0, 5.0, 5.0},
      {5.0, 5.0, 5.0, 5.0, 5.0, 5.0},
      {5.0, 5.0, 5.0, 5.0, 5.0, 5.0},
      {5.0, 5.0, 5.0, 5.0, 5.0, 5.0},
      {5.0, 5.0, 5.0, 5.0, 5.0, 5.0},
      {5.0, 5.0, 5.0, 5.0, 5.0, 5.0},
  };
  config.seed = 42;
  config.horizon_seconds = 10.0;
  config.initial_book.levels_per_side = 5;
  config.initial_book.orders_per_level = 3;
  config.initial_book.best_bid_ticks = 99;
  config.initial_book.best_ask_ticks = 101;

  stochastic::HawkesMarketAdapter adapter;
  const auto run1 = adapter.run(config);

  AnalyticsConfig a_config;
  a_config.currency_per_tick = 0.01;
  a_config.return_horizons_seconds = {0.1, 0.5, 1.0};
  a_config.realized_vol_intervals_seconds = {0.1, 1.0};

  const auto report1 = MicrostructureAnalytics::compute(run1, a_config);

  // Check valid non-trivial metrics
  REQUIRE(report1.quotes.total_observations > 0);
  REQUIRE(report1.quotes.valid_two_sided_count > 0);
  REQUIRE(report1.quotes.spread_ticks.mean > 0.0);
  REQUIRE(report1.trade_intensity.duration_seconds == 10.0);
  REQUIRE(report1.passive_fills.submitted_limit_orders > 0);

  // Verify machine-readable JSON and NDJSON outputs
  const std::string json_str = report1.to_json();
  const std::string ndjson_str = report1.to_ndjson();
  const std::string text_str = report1.to_formatted_report();

  REQUIRE(!json_str.empty());
  REQUIRE(json_str.find("\"quotes\"") != std::string::npos);
  REQUIRE(json_str.find("\"order_flow_imbalance\"") != std::string::npos);
  REQUIRE(!ndjson_str.empty());
  REQUIRE(!text_str.empty());

  // Run 2 with the same seed must produce bitwise identical analytics
  const auto run2 = adapter.run(config);
  const auto report2 = MicrostructureAnalytics::compute(run2, a_config);

  REQUIRE(report1.quotes.spread_ticks.mean == report2.quotes.spread_ticks.mean);
  REQUIRE(report1.ofi.cumulative_ofi_lots == report2.ofi.cumulative_ofi_lots);
  REQUIRE(report1.trade_intensity.trade_count == report2.trade_intensity.trade_count);
  REQUIRE(report1.passive_fills.submitted_limit_orders == report2.passive_fills.submitted_limit_orders);
}
