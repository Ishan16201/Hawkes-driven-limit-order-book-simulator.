#include "lob/stochastic/hawkes_market_adapter.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace lob::stochastic {
namespace {

/// Convert Hawkes time (seconds) to exchange timestamp (nanoseconds).
/// Uses floor to ensure deterministic rounding.
[[nodiscard]] core::TimestampNs seconds_to_ns(const double seconds) noexcept {
  return static_cast<core::TimestampNs>(std::floor(seconds * 1.0e9));
}

[[nodiscard]] core::TimestampNs checked_seconds_to_ns(const double seconds) {
  const double max_seconds =
      static_cast<double>(std::numeric_limits<core::TimestampNs>::max()) / 1.0e9;
  if (!std::isfinite(seconds) || seconds < 0.0 || seconds >= max_seconds) {
    throw std::overflow_error{"Hawkes event time exceeds nanosecond timestamp range"};
  }
  return seconds_to_ns(seconds);
}

/// Draw a uniform integer in [lo, hi] from the given RNG.
[[nodiscard]] core::QuantityLots draw_size(
    const core::QuantityLots lo, const core::QuantityLots hi,
    std::mt19937_64& rng) {
  std::uniform_int_distribution<core::QuantityLots> dist(lo, hi);
  return dist(rng);
}

/// Draw a uniform integer offset in [min_offset, max_offset].
[[nodiscard]] core::PriceTicks draw_offset(
    const core::PriceTicks min_offset, const core::PriceTicks max_offset,
    std::mt19937_64& rng) {
  if (min_offset == max_offset) {
    return min_offset;
  }
  std::uniform_int_distribution<core::PriceTicks> dist(min_offset, max_offset);
  return dist(rng);
}

/// Compute a limit-order price for a buy order.
///
/// Rule: price = anchor - Uniform(min_offset, max_offset)
/// where anchor = best_bid if available, else reference_price.
///
/// The resulting price may cross the best ask. This is intentional:
/// the matching engine will execute the crossing portion.
[[nodiscard]] core::PriceTicks compute_limit_buy_price(
    const exchange::OrderBook& book,
    const PricePlacementConfig& config,
    std::mt19937_64& rng) {
  const auto anchor = book.best_bid().value_or(config.reference_price_ticks);
  const auto offset = draw_offset(config.min_offset_ticks,
                                  config.max_offset_ticks, rng);
  return offset >= anchor ? 1 : anchor - offset;
}

/// Compute a limit-order price for a sell order.
///
/// Rule: price = anchor + Uniform(min_offset, max_offset)
/// where anchor = best_ask if available, else reference_price.
[[nodiscard]] core::PriceTicks compute_limit_sell_price(
    const exchange::OrderBook& book,
    const PricePlacementConfig& config,
    std::mt19937_64& rng) {
  const auto anchor = book.best_ask().value_or(config.reference_price_ticks);
  const auto offset = draw_offset(config.min_offset_ticks,
                                  config.max_offset_ticks, rng);
  if (anchor > std::numeric_limits<core::PriceTicks>::max() - offset) {
    throw std::overflow_error{"limit sell price exceeds tick range"};
  }
  return anchor + offset;
}

}  // namespace

// ---------------------------------------------------------------------------
// Initial book generation
// ---------------------------------------------------------------------------

std::vector<InitialBookOrder> HawkesMarketAdapter::generate_initial_book(
    const InitialBookConfig& config) {
  if (!config.explicit_orders.empty()) {
    return config.explicit_orders;
  }

  if (config.levels_per_side == 0 || config.orders_per_level == 0 ||
      config.order_size == 0 || config.tick_spacing <= 0 ||
      config.best_bid_ticks <= 0 || config.best_ask_ticks <= 0 ||
      config.best_bid_ticks >= config.best_ask_ticks) {
    throw std::invalid_argument{
        "invalid initial book configuration: "
        "require levels>0, orders>0, size>0, spacing>0, bid<ask, both>0"};
  }

  std::vector<InitialBookOrder> orders;
  core::OrderId next_id = 1;
  const auto next_order_id = [&next_id]() {
    if (next_id == 0U) {
      throw std::overflow_error{"initial-book order ID overflow"};
    }
    const core::OrderId id = next_id;
    next_id = id == std::numeric_limits<core::OrderId>::max() ? 0U : id + 1U;
    return id;
  };
  const auto level_offset = [&config](const std::size_t level) {
    const auto max_levels = static_cast<std::uint64_t>(
        std::numeric_limits<core::PriceTicks>::max() /
        config.tick_spacing);
    if (static_cast<std::uint64_t>(level) > max_levels) {
      throw std::overflow_error{"initial-book price offset overflow"};
    }
    return static_cast<core::PriceTicks>(level) * config.tick_spacing;
  };

  // Build bid levels: best_bid, best_bid-spacing, best_bid-2*spacing, ...
  for (std::size_t level = 0; level < config.levels_per_side; ++level) {
    const auto offset = level_offset(level);
    if (offset >= config.best_bid_ticks) {
      break;
    }
    const auto price = config.best_bid_ticks - offset;
    if (price <= 0) {
      break;
    }
    for (std::size_t i = 0; i < config.orders_per_level; ++i) {
      orders.push_back({.id = next_order_id(),
                        .side = core::Side::Buy,
                        .price_ticks = price,
                        .quantity = config.order_size});
    }
  }

  // Build ask levels: best_ask, best_ask+spacing, best_ask+2*spacing, ...
  for (std::size_t level = 0; level < config.levels_per_side; ++level) {
    const auto offset = level_offset(level);
    if (config.best_ask_ticks >
        std::numeric_limits<core::PriceTicks>::max() - offset) {
      throw std::overflow_error{"initial-book ask price overflow"};
    }
    const auto price = config.best_ask_ticks + offset;
    for (std::size_t i = 0; i < config.orders_per_level; ++i) {
      orders.push_back({.id = next_order_id(),
                        .side = core::Side::Sell,
                        .price_ticks = price,
                        .quantity = config.order_size});
    }
  }

  return orders;
}

// ---------------------------------------------------------------------------
// Cancellation target selection
// ---------------------------------------------------------------------------

std::optional<core::OrderId> HawkesMarketAdapter::select_cancel_target(
    const exchange::OrderBook& book,
    const core::Side side,
    const CancelTargetPolicy policy,
    std::mt19937_64& rng) {
  const auto best_price = (side == core::Side::Buy) ? book.best_bid()
                                                    : book.best_ask();
  if (!best_price.has_value()) {
    return std::nullopt;
  }

  switch (policy) {
    case CancelTargetPolicy::BestLevelFront: {
      return book.front_order_id_at(side, *best_price);
    }
    case CancelTargetPolicy::UniformRandom: {
      // Collect all live order IDs on this side by iterating price levels.
      // This is O(orders) but correctness > performance for now.
      std::vector<core::OrderId> candidates;

      // We need to collect orders across all levels on this side.
      // Use the price level iteration approach: start from best,
      // collect all IDs.
      auto collect_from_level = [&](core::PriceTicks price) {
        const auto ids = book.order_ids_at(side, price);
        candidates.insert(candidates.end(), ids.begin(), ids.end());
      };

      // For UniformRandom, we only pick from the best level's queue
      // unless there's a way to iterate all levels. Since OrderBook
      // doesn't expose level iteration, we pick from the best level
      // as a documented simplification.
      //
      // TODO: When OrderBook exposes level iteration, extend to all levels.
      collect_from_level(*best_price);

      if (candidates.empty()) {
        return std::nullopt;
      }
      std::uniform_int_distribution<std::size_t> dist(0,
                                                      candidates.size() - 1);
      return candidates[dist(rng)];
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

void HawkesMarketAdapter::validate(const SyntheticMarketConfig& config) {
  // Validate Hawkes parameters.
  validate_stationary(config.hawkes);

  // Hawkes dimension must match event taxonomy.
  if (config.hawkes.baseline_events_per_second.size() != kEventTypeCount) {
    throw std::invalid_argument{
        "Hawkes dimension must equal kEventTypeCount (6)"};
  }

  // Validate horizon before converting event times to signed nanoseconds.
  const double max_seconds =
      static_cast<double>(std::numeric_limits<core::TimestampNs>::max()) / 1.0e9;
  if (!std::isfinite(config.horizon_seconds) || config.horizon_seconds <= 0.0 ||
      config.horizon_seconds >= max_seconds) {
    throw std::invalid_argument{
        "horizon must be positive, finite, and representable in nanoseconds"};
  }

  // Validate price placement.
  const auto& pp = config.marks.price_placement;
  if (pp.reference_price_ticks <= 0) {
    throw std::invalid_argument{
        "reference_price_ticks must be positive"};
  }
  if (pp.min_offset_ticks < 0 || pp.max_offset_ticks < 0 ||
      pp.min_offset_ticks > pp.max_offset_ticks) {
    throw std::invalid_argument{
        "price placement offsets must be nonneg with min <= max"};
  }

  // Validate limit size.
  const auto& ls = config.marks.limit_size;
  if (ls.min_lots == 0 || ls.min_lots > ls.max_lots) {
    throw std::invalid_argument{
        "limit size must have min > 0 and min <= max"};
  }

  // Validate market size.
  const auto& ms = config.marks.market_size;
  if (ms.min_lots == 0 || ms.min_lots > ms.max_lots) {
    throw std::invalid_argument{
        "market size must have min > 0 and min <= max"};
  }

  // Validate initial book: generate to check consistency.
  // (Will throw if invalid.)
  static_cast<void>(generate_initial_book(config.initial_book));
}

// ---------------------------------------------------------------------------
// Main simulation loop
// ---------------------------------------------------------------------------

SyntheticMarketResult HawkesMarketAdapter::run(
    const SyntheticMarketConfig& config) {
  validate(config);

  // Step 0: Set up matching engine and RNG.
  exchange::MatchingEngine engine;

  // Use the seed for marks, XOR-mixed to avoid correlation with Hawkes RNG.
  std::mt19937_64 marks_rng(config.seed ^ 0x9e3779b97f4a7c15ULL);

  // Step 1: Construct the initial book.
  const auto initial_orders = generate_initial_book(config.initial_book);
  std::optional<core::OrderId> next_order_id{1U};

  for (const auto& order : initial_orders) {
    const auto result = engine.submit_limit(
        order.id, /*timestamp_ns=*/0, order.side, order.price_ticks,
        order.quantity);
    if (!result.accepted) {
      throw std::invalid_argument{
          "initial book order rejected by matching engine"};
    }
    if (next_order_id.has_value() && order.id >= *next_order_id) {
      if (order.id == std::numeric_limits<core::OrderId>::max()) {
        next_order_id.reset();
      } else {
        next_order_id = order.id + 1U;
      }
    }
  }

  const auto take_order_id = [&next_order_id]() {
    if (!next_order_id.has_value()) {
      throw std::overflow_error{"synthetic order ID space exhausted"};
    }
    const core::OrderId id = *next_order_id;
    if (id == std::numeric_limits<core::OrderId>::max()) {
      next_order_id.reset();
    } else {
      ++*next_order_id;
    }
    return id;
  };

  // Step 2: Run the Hawkes process.
  ExponentialHawkesProcess hawkes(config.hawkes, config.seed);
  const auto hawkes_events = hawkes.simulate_until(config.horizon_seconds);

  // Step 3: Process each event through the adapter.
  SyntheticMarketResult result;
  result.config = config;
  result.events.reserve(hawkes_events.size());
  result.book_snapshots.reserve(hawkes_events.size() + 1);
  result.book_snapshots.push_back(engine.snapshot(/*timestamp_ns=*/0));

  for (const auto& he : hawkes_events) {
    const auto event_type = event_type_from_index(he.type);
    const auto time_ns = checked_seconds_to_ns(he.time_seconds);

    MarketEventRecord record;
    record.time_seconds = he.time_seconds;
    record.type = event_type;
    record.pre_event_intensities = he.pre_event_intensities;

    // Count by type.
    ++result.summary.events_by_type[he.type];

    const auto trades_before = engine.trades().size();

    if (is_cancellation(event_type)) {
      // ---- Cancellation event ----
      const auto side = is_buy_side(event_type) ? core::Side::Buy
                                                : core::Side::Sell;
      const auto target = select_cancel_target(
          engine.book(), side, config.marks.cancellation.policy, marks_rng);

      if (!target.has_value()) {
        // No eligible order on this side. Track as inapplicable.
        record.outcome = ActionOutcome::CancellationInapplicable;
        ++result.summary.cancellations_inapplicable;
      } else {
        record.order_id = *target;
        const auto cancel_result = engine.cancel(*target, time_ns);
        if (cancel_result.accepted) {
          record.outcome = ActionOutcome::Accepted;
          record.quantity = cancel_result.cancelled_order
                               ? cancel_result.cancelled_order->remaining_quantity
                               : 0;
          ++result.summary.actions_accepted;
        } else {
          record.outcome = ActionOutcome::Rejected;
          ++result.summary.actions_rejected;
        }
      }
    } else if (is_market_order(event_type)) {
      // ---- Market order event ----
      const auto side = is_buy_side(event_type) ? core::Side::Buy
                                                : core::Side::Sell;
      const auto size = draw_size(config.marks.market_size.min_lots,
                                  config.marks.market_size.max_lots,
                                  marks_rng);
      const auto oid = take_order_id();
      record.order_id = oid;
      record.quantity = size;

      const auto sub_result = engine.submit_market(oid, time_ns, side, size);
      if (sub_result.accepted) {
        record.outcome = ActionOutcome::Accepted;
        ++result.summary.actions_accepted;
      } else {
        record.outcome = ActionOutcome::Rejected;
        ++result.summary.actions_rejected;
      }
    } else {
      // ---- Limit order event ----
      const auto is_buy = is_buy_side(event_type);
      const auto side = is_buy ? core::Side::Buy : core::Side::Sell;
      const auto size = draw_size(config.marks.limit_size.min_lots,
                                  config.marks.limit_size.max_lots,
                                  marks_rng);
      const auto price = is_buy
                             ? compute_limit_buy_price(
                                   engine.book(),
                                   config.marks.price_placement, marks_rng)
                             : compute_limit_sell_price(
                                   engine.book(),
                                   config.marks.price_placement, marks_rng);
      const auto oid = take_order_id();
      record.order_id = oid;
      record.price_ticks = price;
      record.quantity = size;

      const auto sub_result =
          engine.submit_limit(oid, time_ns, side, price, size);
      if (sub_result.accepted) {
        record.outcome = ActionOutcome::Accepted;
        ++result.summary.actions_accepted;
      } else {
        record.outcome = ActionOutcome::Rejected;
        ++result.summary.actions_rejected;
      }
    }

    record.trades_generated = engine.trades().size() - trades_before;
    result.events.push_back(std::move(record));
    result.book_snapshots.push_back(engine.snapshot(time_ns));
  }

  // Step 4: Assemble final results.
  result.trades = engine.trades();
  result.summary.hawkes_events = result.events.size();
  result.summary.trades = result.trades.size();
  for (const auto& t : result.trades) {
    if (std::numeric_limits<core::QuantityLots>::max() -
            result.summary.traded_quantity <
        t.quantity) {
      throw std::overflow_error{"synthetic traded-quantity summary overflow"};
    }
    result.summary.traded_quantity += t.quantity;
  }
  result.summary.final_live_orders = engine.book().live_order_count();
  result.summary.final_best_bid = engine.book().best_bid();
  result.summary.final_best_ask = engine.book().best_ask();

  return result;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

std::string SyntheticMarketResult::event_log_ndjson() const {
  std::ostringstream out;
  for (const auto& record : events) {
    const char* outcome = "rejected";
    switch (record.outcome) {
      case ActionOutcome::Accepted: outcome = "accepted"; break;
      case ActionOutcome::Rejected: outcome = "rejected"; break;
      case ActionOutcome::CancellationInapplicable:
        outcome = "cancellation_inapplicable";
        break;
    }
    out << "{\"record_type\":\"market_event\",\"schema_version\":1"
        << ",\"time_seconds\":" << record.time_seconds
        << ",\"timestamp_ns\":" << seconds_to_ns(record.time_seconds)
        << ",\"event_type\":\""
        << kEventTypeLabels[event_type_index(record.type)]
        << "\",\"outcome\":\"" << outcome
        << "\",\"order_id\":" << record.order_id
        << ",\"price_ticks\":" << record.price_ticks
        << ",\"quantity_lots\":" << record.quantity
        << ",\"trades_generated\":" << record.trades_generated
        << ",\"pre_event_intensities\":[";
    for (std::size_t i = 0; i < record.pre_event_intensities.size(); ++i) {
      if (i > 0U) out << ",";
      out << record.pre_event_intensities[i];
    }
    out << "]}\n";
  }
  return out.str();
}

std::string SyntheticMarketResult::trades_ndjson() const {
  std::ostringstream out;
  for (const auto& trade : trades) {
    out << "{\"record_type\":\"trade\",\"schema_version\":1"
        << ",\"trade_id\":" << trade.id
        << ",\"timestamp_ns\":" << trade.timestamp_ns
        << ",\"price_ticks\":" << trade.price_ticks
        << ",\"quantity_lots\":" << trade.quantity
        << ",\"aggressor_side\":\""
        << (trade.aggressor_side == core::Side::Buy ? "BUY" : "SELL")
        << "\",\"resting_order_id\":" << trade.resting_order_id
        << ",\"incoming_order_id\":" << trade.incoming_order_id
        << ",\"resting_remaining_quantity_lots\":"
        << trade.resting_remaining_quantity
        << ",\"incoming_remaining_quantity_lots\":"
        << trade.incoming_remaining_quantity << "}\n";
  }
  return out.str();
}

std::string SyntheticMarketResult::run_manifest_json() const {
  std::ostringstream out;
  out << std::setprecision(17)
      << "{\n"
      << "  \"manifest_schema_version\": 1,\n"
      << "  \"event_schema_version\": 1,\n"
      << "  \"model\": \"multivariate exponential Hawkes\",\n"
      << "  \"time_unit\": \"seconds; exchange timestamps are floored nanoseconds\",\n"
      << "  \"price_unit\": \"ticks\",\n"
      << "  \"quantity_unit\": \"lots\",\n"
      << "  \"seed\": " << config.seed << ",\n"
      << "  \"horizon_seconds\": " << config.horizon_seconds << ",\n"
      << "  \"event_count\": " << summary.hawkes_events << ",\n"
      << "  \"trade_count\": " << summary.trades << ",\n"
      << "  \"hawkes\": {\n"
      << "    \"baseline_events_per_second\": [";
  for (std::size_t i = 0;
       i < config.hawkes.baseline_events_per_second.size(); ++i) {
    if (i > 0U) out << ",";
    out << config.hawkes.baseline_events_per_second[i];
  }
  out << "],\n    \"alpha_branching\": [";
  for (std::size_t i = 0; i < config.hawkes.alpha_branching.size(); ++i) {
    if (i > 0U) out << ",";
    out << "[";
    for (std::size_t j = 0; j < config.hawkes.alpha_branching[i].size(); ++j) {
      if (j > 0U) out << ",";
      out << config.hawkes.alpha_branching[i][j];
    }
    out << "]";
  }
  out << "],\n    \"beta_per_second\": [";
  for (std::size_t i = 0; i < config.hawkes.beta_per_second.size(); ++i) {
    if (i > 0U) out << ",";
    out << "[";
    for (std::size_t j = 0; j < config.hawkes.beta_per_second[i].size(); ++j) {
      if (j > 0U) out << ",";
      out << config.hawkes.beta_per_second[i][j];
    }
    out << "]";
  }
  out << "]\n  },\n"
      << "  \"marks\": {\n"
      << "    \"reference_price_ticks\": "
      << config.marks.price_placement.reference_price_ticks << ",\n"
      << "    \"min_offset_ticks\": "
      << config.marks.price_placement.min_offset_ticks << ",\n"
      << "    \"max_offset_ticks\": "
      << config.marks.price_placement.max_offset_ticks << ",\n"
      << "    \"limit_size_lots\": ["
      << config.marks.limit_size.min_lots << ","
      << config.marks.limit_size.max_lots << "],\n"
      << "    \"market_size_lots\": ["
      << config.marks.market_size.min_lots << ","
      << config.marks.market_size.max_lots << "],\n"
      << "    \"cancel_target_policy\": \""
      << (config.marks.cancellation.policy == CancelTargetPolicy::BestLevelFront
              ? "best_level_front"
              : "uniform_random_best_level")
      << "\"\n  },\n"
      << "  \"initial_book\": {\n"
      << "    \"best_bid_ticks\": " << config.initial_book.best_bid_ticks
      << ",\n"
      << "    \"best_ask_ticks\": " << config.initial_book.best_ask_ticks
      << ",\n"
      << "    \"levels_per_side\": " << config.initial_book.levels_per_side
      << ",\n"
      << "    \"orders_per_level\": "
      << config.initial_book.orders_per_level << ",\n"
      << "    \"tick_spacing\": " << config.initial_book.tick_spacing
      << ",\n"
      << "    \"order_size_lots\": " << config.initial_book.order_size
      << ",\n"
      << "    \"explicit_orders\": [";
  for (std::size_t i = 0; i < config.initial_book.explicit_orders.size(); ++i) {
    if (i > 0U) out << ",";
    const auto& order = config.initial_book.explicit_orders[i];
    out << "{\"id\":" << order.id << ",\"side\":\""
        << (order.side == core::Side::Buy ? "buy" : "sell")
        << "\",\"price_ticks\":" << order.price_ticks
        << ",\"quantity_lots\":" << order.quantity << "}";
  }
  out << "]\n  },\n"
      << "  \"empirical_validation\": \"not_performed\"\n"
      << "}\n";
  return out.str();
}

std::string SyntheticMarketResult::summary_report() const {
  std::ostringstream out;
  out << "=== Synthetic Market Summary ===\n"
      << "Status: illustrative simulation (not empirically calibrated)\n\n"
      << "Hawkes events:   " << summary.hawkes_events << "\n"
      << "  Accepted:      " << summary.actions_accepted << "\n"
      << "  Rejected:      " << summary.actions_rejected << "\n"
      << "  Cancel N/A:    " << summary.cancellations_inapplicable << "\n"
      << "Trades:          " << summary.trades << "\n"
      << "Traded quantity: " << summary.traded_quantity << " lots\n\n"
      << "Events by type:\n";

  for (std::size_t i = 0; i < kEventTypeCount; ++i) {
    out << "  " << kEventTypeLabels[i] << ": "
        << summary.events_by_type[i] << "\n";
  }

  out << "\nFinal book:\n"
      << "  Live orders: " << summary.final_live_orders << "\n"
      << "  Best bid:    ";
  if (summary.final_best_bid) {
    out << *summary.final_best_bid;
  } else {
    out << "(empty)";
  }
  out << "\n  Best ask:    ";
  if (summary.final_best_ask) {
    out << *summary.final_best_ask;
  } else {
    out << "(empty)";
  }
  out << "\n";

  return out.str();
}

}  // namespace lob::stochastic
