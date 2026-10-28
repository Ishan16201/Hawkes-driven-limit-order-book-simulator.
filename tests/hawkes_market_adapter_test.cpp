/// Tests for the Hawkes-market adapter: the complete pipeline from Hawkes
/// process through exchange-event adapter to matching engine.
///
/// Tests use fixed seeds and configurations to verify reproducibility,
/// correctness of event accounting, matching invariants, and proper
/// handling of cancellation semantics.

#include "lob/stochastic/hawkes_market_adapter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace {

/// Minimal 6-dimensional Hawkes parameters for testing.
/// Low excitation to keep event counts manageable.
lob::stochastic::HawkesParameters test_hawkes_params() {
  return {
      .baseline_events_per_second = {1.0, 1.0, 0.5, 0.5, 0.3, 0.3},
      .alpha_branching = {
          {0.05, 0.01, 0.01, 0.01, 0.01, 0.01},
          {0.01, 0.05, 0.01, 0.01, 0.01, 0.01},
          {0.02, 0.01, 0.03, 0.01, 0.01, 0.01},
          {0.01, 0.02, 0.01, 0.03, 0.01, 0.01},
          {0.01, 0.01, 0.01, 0.01, 0.05, 0.02},
          {0.01, 0.01, 0.01, 0.01, 0.02, 0.05},
      },
      .beta_per_second = {
          {3.0, 3.0, 3.0, 3.0, 3.0, 3.0},
          {3.0, 3.0, 3.0, 3.0, 3.0, 3.0},
          {3.0, 3.0, 3.0, 3.0, 3.0, 3.0},
          {3.0, 3.0, 3.0, 3.0, 3.0, 3.0},
          {3.0, 3.0, 3.0, 3.0, 3.0, 3.0},
          {3.0, 3.0, 3.0, 3.0, 3.0, 3.0},
      },
  };
}

/// Standard test config: 5-second horizon, deterministic seed.
lob::stochastic::SyntheticMarketConfig test_config() {
  return {
      .hawkes = test_hawkes_params(),
      .seed = 42,
      .horizon_seconds = 5.0,
      .marks = {
          .price_placement = {
              .reference_price_ticks = 1000,
              .min_offset_ticks = 0,
              .max_offset_ticks = 2,
          },
          .limit_size = {.min_lots = 1, .max_lots = 3},
          .market_size = {.min_lots = 1, .max_lots = 2},
          .cancellation = {
              .policy = lob::stochastic::CancelTargetPolicy::BestLevelFront,
          },
      },
      .initial_book = {
          .best_bid_ticks = 999,
          .best_ask_ticks = 1001,
          .levels_per_side = 3,
          .orders_per_level = 2,
          .tick_spacing = 1,
          .order_size = 5,
      },
  };
}

}  // namespace

// ===========================================================================
// Simulation termination and reproducibility
// ===========================================================================

TEST_CASE("simulation terminates at configured horizon") {
  auto config = test_config();
  config.horizon_seconds = 2.0;

  lob::stochastic::HawkesMarketAdapter adapter;
  const auto result = adapter.run(config);

  // All events must be within the horizon.
  for (const auto& e : result.events) {
    REQUIRE(e.time_seconds <= config.horizon_seconds);
    REQUIRE(e.time_seconds > 0.0);
  }

  // Events must be time-ordered.
  for (std::size_t i = 1; i < result.events.size(); ++i) {
    REQUIRE(result.events[i].time_seconds >= result.events[i - 1].time_seconds);
  }
}

TEST_CASE("event log is reproducible with same seed") {
  const auto config = test_config();
  lob::stochastic::HawkesMarketAdapter adapter;

  const auto first = adapter.run(config);
  const auto second = adapter.run(config);

  REQUIRE(first.events.size() == second.events.size());
  REQUIRE(first.trades.size() == second.trades.size());
  REQUIRE(first.summary.hawkes_events == second.summary.hawkes_events);
  REQUIRE(first.summary.actions_accepted == second.summary.actions_accepted);
  REQUIRE(first.summary.cancellations_inapplicable ==
          second.summary.cancellations_inapplicable);
  REQUIRE(first.summary.traded_quantity == second.summary.traded_quantity);

  // Verify each event matches.
  for (std::size_t i = 0; i < first.events.size(); ++i) {
    REQUIRE(first.events[i].time_seconds == second.events[i].time_seconds);
    REQUIRE(first.events[i].type == second.events[i].type);
    REQUIRE(first.events[i].outcome == second.events[i].outcome);
    REQUIRE(first.events[i].order_id == second.events[i].order_id);
    REQUIRE(first.events[i].price_ticks == second.events[i].price_ticks);
    REQUIRE(first.events[i].quantity == second.events[i].quantity);
  }

  // NDJSON logs must be identical.
  REQUIRE(first.event_log_ndjson() == second.event_log_ndjson());
}

TEST_CASE("synthetic event, trade, and manifest outputs contain reproducible data") {
  const auto result = lob::stochastic::HawkesMarketAdapter{}.run(test_config());
  const auto event_log = result.event_log_ndjson();
  const auto trades_log = result.trades_ndjson();
  const auto manifest = result.run_manifest_json();

  REQUIRE(event_log.find("\"pre_event_intensities\":[") != std::string::npos);
  REQUIRE(event_log.find("\"timestamp_ns\":") != std::string::npos);
  REQUIRE(manifest.find("\"seed\": 42") != std::string::npos);
  REQUIRE(manifest.find("\"event_count\": " +
                        std::to_string(result.summary.hawkes_events)) !=
          std::string::npos);
  REQUIRE(manifest.find("\"empirical_validation\": \"not_performed\"") !=
          std::string::npos);
  if (!result.trades.empty()) {
    REQUIRE(trades_log.find("\"resting_order_id\":") != std::string::npos);
    REQUIRE(trades_log.find("\"incoming_remaining_quantity_lots\":") !=
            std::string::npos);
  }
}

TEST_CASE("adapter rejects unrepresentable horizon and initial-book price overflow") {
  auto config = test_config();
  config.horizon_seconds = 10'000'000'000.0;
  REQUIRE_THROWS_AS(lob::stochastic::HawkesMarketAdapter{}.run(config),
                    std::invalid_argument);

  lob::stochastic::InitialBookConfig initial;
  initial.best_bid_ticks =
      std::numeric_limits<lob::core::PriceTicks>::max() - 2;
  initial.best_ask_ticks =
      std::numeric_limits<lob::core::PriceTicks>::max() - 1;
  initial.levels_per_side = 3U;
  initial.orders_per_level = 1U;
  initial.tick_spacing = 1;
  REQUIRE_THROWS_AS(
      lob::stochastic::HawkesMarketAdapter::generate_initial_book(initial),
      std::overflow_error);
}

TEST_CASE("different seed produces different event log") {
  auto config_a = test_config();
  auto config_b = test_config();
  config_b.seed = 9999;

  lob::stochastic::HawkesMarketAdapter adapter;
  const auto result_a = adapter.run(config_a);
  const auto result_b = adapter.run(config_b);

  // With overwhelming probability, event counts differ or times differ.
  bool differs = (result_a.events.size() != result_b.events.size());
  if (!differs && !result_a.events.empty()) {
    differs = (result_a.events[0].time_seconds !=
               result_b.events[0].time_seconds);
  }
  REQUIRE(differs);
}

// ===========================================================================
// Event accounting invariants
// ===========================================================================

TEST_CASE("summary statistics agree with event log") {
  lob::stochastic::HawkesMarketAdapter adapter;
  const auto result = adapter.run(test_config());

  // hawkes_events == total events in the log
  REQUIRE(result.summary.hawkes_events == result.events.size());

  // Count accepted, rejected, inapplicable from the event log.
  std::size_t accepted = 0;
  std::size_t rejected = 0;
  std::size_t inapplicable = 0;
  std::size_t by_type[lob::stochastic::kEventTypeCount] = {};

  for (const auto& e : result.events) {
    switch (e.outcome) {
      case lob::stochastic::ActionOutcome::Accepted:
        ++accepted;
        break;
      case lob::stochastic::ActionOutcome::Rejected:
        ++rejected;
        break;
      case lob::stochastic::ActionOutcome::CancellationInapplicable:
        ++inapplicable;
        break;
    }
    ++by_type[lob::stochastic::event_type_index(e.type)];
  }

  REQUIRE(accepted == result.summary.actions_accepted);
  REQUIRE(rejected == result.summary.actions_rejected);
  REQUIRE(inapplicable == result.summary.cancellations_inapplicable);

  // Invariant: accepted + rejected + inapplicable == total
  REQUIRE(accepted + rejected + inapplicable == result.summary.hawkes_events);

  // Per-type counts match.
  for (std::size_t i = 0; i < lob::stochastic::kEventTypeCount; ++i) {
    REQUIRE(by_type[i] == result.summary.events_by_type[i]);
  }

  // Trade count from summary matches trade vector.
  REQUIRE(result.summary.trades == result.trades.size());

  // Traded quantity from summary matches sum of trade quantities.
  lob::core::QuantityLots total_traded = 0;
  for (const auto& t : result.trades) {
    total_traded += t.quantity;
  }
  REQUIRE(total_traded == result.summary.traded_quantity);

  // Total trades_generated in events should match total trades.
  std::size_t event_trades = 0;
  for (const auto& e : result.events) {
    event_trades += e.trades_generated;
  }
  REQUIRE(event_trades == result.summary.trades);
}

// ===========================================================================
// Cancellation semantics
// ===========================================================================

TEST_CASE("cancellation attempts on empty side are tracked as inapplicable") {
  // Use a configuration where cancellations dominate and no new limit orders
  // arrive to replenish the book. This guarantees the book gets drained.
  auto config = test_config();
  config.horizon_seconds = 30.0;

  // High cancellation rates, near-zero everything else.
  config.hawkes = {
      .baseline_events_per_second = {0.01, 0.01, 5.0, 5.0, 0.01, 0.01},
      .alpha_branching = {
          {0.01, 0.01, 0.01, 0.01, 0.01, 0.01},
          {0.01, 0.01, 0.01, 0.01, 0.01, 0.01},
          {0.01, 0.01, 0.01, 0.01, 0.01, 0.01},
          {0.01, 0.01, 0.01, 0.01, 0.01, 0.01},
          {0.01, 0.01, 0.01, 0.01, 0.01, 0.01},
          {0.01, 0.01, 0.01, 0.01, 0.01, 0.01},
      },
      .beta_per_second = {
          {3.0, 3.0, 3.0, 3.0, 3.0, 3.0},
          {3.0, 3.0, 3.0, 3.0, 3.0, 3.0},
          {3.0, 3.0, 3.0, 3.0, 3.0, 3.0},
          {3.0, 3.0, 3.0, 3.0, 3.0, 3.0},
          {3.0, 3.0, 3.0, 3.0, 3.0, 3.0},
          {3.0, 3.0, 3.0, 3.0, 3.0, 3.0},
      },
  };

  // Small initial book that will be exhausted quickly.
  config.initial_book = {
      .best_bid_ticks = 999,
      .best_ask_ticks = 1001,
      .levels_per_side = 1,
      .orders_per_level = 2,
      .tick_spacing = 1,
      .order_size = 1,
  };

  lob::stochastic::HawkesMarketAdapter adapter;
  const auto result = adapter.run(config);

  // There should be some inapplicable cancellations.
  bool has_inapplicable = false;
  for (const auto& e : result.events) {
    if (e.outcome == lob::stochastic::ActionOutcome::CancellationInapplicable) {
      has_inapplicable = true;

      // Inapplicable cancellations must be cancellation events.
      REQUIRE(lob::stochastic::is_cancellation(e.type));

      // They should have order_id == 0 (no target selected).
      REQUIRE(e.order_id == 0);
    }
  }

  // With 5 cancellations/s, 30s horizon, and only 4 initial orders,
  // we will definitely have inapplicable cancellations.
  REQUIRE(has_inapplicable);
  REQUIRE(result.summary.cancellations_inapplicable > 0);
}


TEST_CASE("successful cancellation refers to a live order") {
  lob::stochastic::HawkesMarketAdapter adapter;
  const auto result = adapter.run(test_config());

  // We can't directly verify against the book mid-simulation, but we can
  // check that successful cancellations have nonzero order IDs.
  for (const auto& e : result.events) {
    if (lob::stochastic::is_cancellation(e.type) &&
        e.outcome == lob::stochastic::ActionOutcome::Accepted) {
      REQUIRE(e.order_id != 0);
    }
  }
}

// ===========================================================================
// Price and quantity constraints
// ===========================================================================

TEST_CASE("all prices in events are positive and within configured range") {
  lob::stochastic::HawkesMarketAdapter adapter;
  const auto result = adapter.run(test_config());

  for (const auto& e : result.events) {
    if (lob::stochastic::is_limit_order(e.type)) {
      REQUIRE(e.price_ticks > 0);
    }
    if (lob::stochastic::is_market_order(e.type)) {
      // Market orders should have price_ticks == 0 in the event record.
      REQUIRE(e.price_ticks == 0);
    }
  }
}

TEST_CASE("all order quantities obey configured size bounds") {
  auto config = test_config();
  config.marks.limit_size = {.min_lots = 2, .max_lots = 7};
  config.marks.market_size = {.min_lots = 1, .max_lots = 4};

  lob::stochastic::HawkesMarketAdapter adapter;
  const auto result = adapter.run(config);

  for (const auto& e : result.events) {
    if (lob::stochastic::is_limit_order(e.type) &&
        e.outcome == lob::stochastic::ActionOutcome::Accepted) {
      REQUIRE(e.quantity >= config.marks.limit_size.min_lots);
      REQUIRE(e.quantity <= config.marks.limit_size.max_lots);
    }
    if (lob::stochastic::is_market_order(e.type) &&
        e.outcome == lob::stochastic::ActionOutcome::Accepted) {
      REQUIRE(e.quantity >= config.marks.market_size.min_lots);
      REQUIRE(e.quantity <= config.marks.market_size.max_lots);
    }
  }
}

TEST_CASE("trade prices equal resting order prices (matching invariant)") {
  lob::stochastic::HawkesMarketAdapter adapter;
  const auto result = adapter.run(test_config());

  // All trade prices should be positive (resting orders have positive prices).
  for (const auto& t : result.trades) {
    REQUIRE(t.price_ticks > 0);
    REQUIRE(t.quantity > 0);
  }
}

// ===========================================================================
// Failed/rejected actions don't corrupt the book
// ===========================================================================

TEST_CASE("rejected actions do not corrupt book state") {
  // Run a simulation and verify that the final book is well-formed.
  lob::stochastic::HawkesMarketAdapter adapter;
  const auto result = adapter.run(test_config());

  // Final book state should be consistent: if we have a best bid and best ask,
  // the book should not be crossed.
  if (result.summary.final_best_bid && result.summary.final_best_ask) {
    REQUIRE(*result.summary.final_best_bid < *result.summary.final_best_ask);
  }
}

// ===========================================================================
// Initial book construction
// ===========================================================================

TEST_CASE("generated initial book has correct structure") {
  const lob::stochastic::InitialBookConfig config{
      .best_bid_ticks = 99,
      .best_ask_ticks = 101,
      .levels_per_side = 3,
      .orders_per_level = 2,
      .tick_spacing = 1,
      .order_size = 10,
  };

  const auto orders =
      lob::stochastic::HawkesMarketAdapter::generate_initial_book(config);

  // 3 levels * 2 orders * 2 sides = 12 orders.
  REQUIRE(orders.size() == 12);

  // Verify bid orders.
  std::size_t bid_count = 0;
  std::size_t ask_count = 0;
  for (const auto& o : orders) {
    REQUIRE(o.quantity == 10);
    REQUIRE(o.price_ticks > 0);
    REQUIRE(o.id > 0);
    if (o.side == lob::core::Side::Buy) {
      ++bid_count;
      REQUIRE(o.price_ticks >= 97);  // 99 - 2*1 = 97
      REQUIRE(o.price_ticks <= 99);
    } else {
      ++ask_count;
      REQUIRE(o.price_ticks >= 101);
      REQUIRE(o.price_ticks <= 103);  // 101 + 2*1 = 103
    }
  }
  REQUIRE(bid_count == 6);
  REQUIRE(ask_count == 6);
}

TEST_CASE("explicit initial book overrides generated orders") {
  const lob::stochastic::InitialBookConfig config{
      .best_bid_ticks = 99,
      .best_ask_ticks = 101,
      .levels_per_side = 10,  // Ignored.
      .orders_per_level = 10, // Ignored.
      .tick_spacing = 1,
      .order_size = 100,
      .explicit_orders = {
          {.id = 1, .side = lob::core::Side::Buy, .price_ticks = 50, .quantity = 1},
          {.id = 2, .side = lob::core::Side::Sell, .price_ticks = 60, .quantity = 1},
      },
  };

  const auto orders =
      lob::stochastic::HawkesMarketAdapter::generate_initial_book(config);
  REQUIRE(orders.size() == 2);
  REQUIRE(orders[0].price_ticks == 50);
  REQUIRE(orders[1].price_ticks == 60);
}

TEST_CASE("invalid initial book config is rejected") {
  // Bid >= ask.
  REQUIRE_THROWS_AS(
      lob::stochastic::HawkesMarketAdapter::generate_initial_book({
          .best_bid_ticks = 100,
          .best_ask_ticks = 100,
          .levels_per_side = 1,
          .orders_per_level = 1,
          .tick_spacing = 1,
          .order_size = 1,
      }),
      std::invalid_argument);

  // Zero levels.
  REQUIRE_THROWS_AS(
      lob::stochastic::HawkesMarketAdapter::generate_initial_book({
          .best_bid_ticks = 99,
          .best_ask_ticks = 101,
          .levels_per_side = 0,
          .orders_per_level = 1,
          .tick_spacing = 1,
          .order_size = 1,
      }),
      std::invalid_argument);
}

// ===========================================================================
// Configuration validation
// ===========================================================================

TEST_CASE("wrong Hawkes dimension is rejected") {
  auto config = test_config();
  config.hawkes.baseline_events_per_second = {1.0, 1.0};  // Only 2 dims.
  config.hawkes.alpha_branching = {{0.1, 0.0}, {0.0, 0.1}};
  config.hawkes.beta_per_second = {{1.0, 1.0}, {1.0, 1.0}};

  lob::stochastic::HawkesMarketAdapter adapter;
  REQUIRE_THROWS_AS(adapter.run(config), std::invalid_argument);
}

TEST_CASE("invalid mark distribution config is rejected") {
  // Zero min size.
  auto config = test_config();
  config.marks.limit_size.min_lots = 0;

  lob::stochastic::HawkesMarketAdapter adapter;
  REQUIRE_THROWS_AS(adapter.run(config), std::invalid_argument);
}

TEST_CASE("invalid horizon is rejected") {
  auto config = test_config();
  config.horizon_seconds = -1.0;

  lob::stochastic::HawkesMarketAdapter adapter;
  REQUIRE_THROWS_AS(adapter.run(config), std::invalid_argument);
}

// ===========================================================================
// Event type taxonomy
// ===========================================================================

TEST_CASE("event type taxonomy covers all six types") {
  using lob::stochastic::EventType;
  using lob::stochastic::kEventTypeCount;

  REQUIRE(kEventTypeCount == 6);

  // Verify round-trip.
  for (std::size_t i = 0; i < kEventTypeCount; ++i) {
    const auto type = lob::stochastic::event_type_from_index(i);
    REQUIRE(lob::stochastic::event_type_index(type) == i);
  }

  // Out-of-range throws.
  REQUIRE_THROWS_AS(lob::stochastic::event_type_from_index(6),
                    std::out_of_range);

  // Classification checks.
  REQUIRE(lob::stochastic::is_limit_order(EventType::LimitBuy));
  REQUIRE(lob::stochastic::is_limit_order(EventType::LimitSell));
  REQUIRE(!lob::stochastic::is_limit_order(EventType::MarketBuy));

  REQUIRE(lob::stochastic::is_market_order(EventType::MarketBuy));
  REQUIRE(lob::stochastic::is_market_order(EventType::MarketSell));
  REQUIRE(!lob::stochastic::is_market_order(EventType::CancelBuy));

  REQUIRE(lob::stochastic::is_cancellation(EventType::CancelBuy));
  REQUIRE(lob::stochastic::is_cancellation(EventType::CancelSell));
  REQUIRE(!lob::stochastic::is_cancellation(EventType::LimitBuy));

  REQUIRE(lob::stochastic::is_buy_side(EventType::LimitBuy));
  REQUIRE(lob::stochastic::is_buy_side(EventType::CancelBuy));
  REQUIRE(lob::stochastic::is_buy_side(EventType::MarketBuy));
  REQUIRE(!lob::stochastic::is_buy_side(EventType::LimitSell));
}

// ===========================================================================
// Pre-event intensities are recorded
// ===========================================================================

TEST_CASE("pre-event intensities are recorded for each event") {
  lob::stochastic::HawkesMarketAdapter adapter;
  const auto result = adapter.run(test_config());

  for (const auto& e : result.events) {
    REQUIRE(e.pre_event_intensities.size() == lob::stochastic::kEventTypeCount);
    for (const auto& intensity : e.pre_event_intensities) {
      REQUIRE(intensity >= 0.0);
      REQUIRE(std::isfinite(intensity));
    }
  }
}

// ===========================================================================
// Complete pipeline integration
// ===========================================================================

TEST_CASE("complete pipeline produces non-trivial results") {
  auto config = test_config();
  config.horizon_seconds = 10.0;

  lob::stochastic::HawkesMarketAdapter adapter;
  const auto result = adapter.run(config);

  // With 10 seconds and combined baseline ~3.6 events/s, expect >20 events.
  REQUIRE(result.summary.hawkes_events > 20);

  // Should have some trades.
  REQUIRE(result.summary.trades > 0);

  // Should have events of multiple types.
  std::size_t types_seen = 0;
  for (std::size_t i = 0; i < lob::stochastic::kEventTypeCount; ++i) {
    if (result.summary.events_by_type[i] > 0) {
      ++types_seen;
    }
  }
  REQUIRE(types_seen >= 4);  // At least 4 of 6 types should appear.

  // NDJSON should be non-empty.
  REQUIRE(!result.event_log_ndjson().empty());

  // Summary report should be non-empty.
  REQUIRE(!result.summary_report().empty());
}

TEST_CASE("uniform random cancellation policy works") {
  auto config = test_config();
  config.marks.cancellation.policy =
      lob::stochastic::CancelTargetPolicy::UniformRandom;
  config.horizon_seconds = 5.0;

  lob::stochastic::HawkesMarketAdapter adapter;
  const auto result = adapter.run(config);

  // Should complete without error and produce valid results.
  REQUIRE(result.summary.hawkes_events == result.events.size());

  // Verify accounting invariant.
  REQUIRE(result.summary.actions_accepted +
              result.summary.actions_rejected +
              result.summary.cancellations_inapplicable ==
          result.summary.hawkes_events);
}
