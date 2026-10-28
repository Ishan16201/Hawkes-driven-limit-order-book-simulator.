#pragma once

/// Exchange-event adapter: maps stochastic Hawkes event types to concrete
/// exchange actions (orders and cancellations).
///
/// Architecture:
///   HawkesProcess  -->  EventType  -->  HawkesMarketAdapter  -->  MatchingEngine
///                     (category)         (generates marks:        (deterministic
///                                         price, size, target)     processing)
///
/// The adapter is a separate layer between the stochastic generator and the
/// deterministic exchange. The exchange never depends on Hawkes classes.
///
/// Mark distributions (price placement, order size, cancellation target) are
/// configured independently from the Hawkes intensity model through the
/// MarkDistributionConfig struct.

#include "lob/core/types.hpp"
#include "lob/exchange/matching_engine.hpp"
#include "lob/stochastic/event_types.hpp"
#include "lob/stochastic/hawkes_parameters.hpp"
#include "lob/stochastic/hawkes_process.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace lob::stochastic {

// ---------------------------------------------------------------------------
// Mark distribution configuration
// ---------------------------------------------------------------------------

/// Controls how limit-order prices are placed relative to the current best
/// bid/ask. Prices are drawn as:
///   buy:  best_bid - Uniform(min_offset_ticks, max_offset_ticks)
///   sell: best_ask + Uniform(min_offset_ticks, max_offset_ticks)
///
/// If no best bid/ask exists, reference_price_ticks is used as the anchor.
///
/// The matching engine will execute crossing orders, so if a newly submitted
/// limit buy price >= best ask, it will trade. This is intentional — the
/// matching engine resolves all crossing, and the adapter never silently
/// prevents it.
///
/// Units: integer ticks. All offsets are >= 0.
struct PricePlacementConfig final {
  /// Fallback anchor when the relevant side of the book is empty.
  core::PriceTicks reference_price_ticks{100};

  /// Minimum offset from the best quote (inclusive). 0 means "at the best".
  core::PriceTicks min_offset_ticks{0};

  /// Maximum offset from the best quote (inclusive).
  core::PriceTicks max_offset_ticks{5};
};

/// Controls the distribution of order sizes for limit and market orders.
/// Sizes are drawn from Uniform(min_lots, max_lots).
///
/// Units: integer lots.
struct SizeDistributionConfig final {
  core::QuantityLots min_lots{1};
  core::QuantityLots max_lots{10};
};

/// Policy for selecting which live order to cancel when a cancellation
/// event fires.
enum class CancelTargetPolicy : std::uint8_t {
  /// Cancel the order at the front of the best price level (FIFO-first).
  BestLevelFront,

  /// Cancel a uniformly random live order on the relevant side.
  UniformRandom,
};

/// Controls cancellation target selection and failed-cancellation handling.
struct CancellationConfig final {
  CancelTargetPolicy policy{CancelTargetPolicy::BestLevelFront};
};

/// Controls how market-order sizes are generated. Separated from limit sizes
/// because aggressive and passive flows may have different size profiles.
struct MarketSizeConfig final {
  core::QuantityLots min_lots{1};
  core::QuantityLots max_lots{5};
};

/// Complete mark distribution configuration, independent of Hawkes parameters.
///
/// Each rule is documented and configurable. The Hawkes intensity model only
/// determines *when* and *what type* of event occurs; these rules determine
/// the concrete order details.
struct MarkDistributionConfig final {
  PricePlacementConfig price_placement{};
  SizeDistributionConfig limit_size{};
  MarketSizeConfig market_size{};
  CancellationConfig cancellation{};
};

// ---------------------------------------------------------------------------
// Initial book construction
// ---------------------------------------------------------------------------

/// A single order in the initial book snapshot.
struct InitialBookOrder final {
  core::OrderId id{};
  core::Side side{core::Side::Buy};
  core::PriceTicks price_ticks{};
  core::QuantityLots quantity{};
};

/// Configuration for the documented initial-book construction procedure.
///
/// Strategy: place `levels_per_side` price levels on each side, with
/// `orders_per_level` orders at each level, starting from `best_bid_ticks`
/// and `best_ask_ticks` and moving outward by `tick_spacing`.
///
/// If `explicit_orders` is non-empty, it is used instead of the generated
/// book. This allows hand-crafted initial configurations.
struct InitialBookConfig final {
  /// Starting price for the bid side (highest bid).
  core::PriceTicks best_bid_ticks{99};

  /// Starting price for the ask side (lowest ask).
  core::PriceTicks best_ask_ticks{101};

  /// Number of price levels on each side.
  std::size_t levels_per_side{5};

  /// Number of orders at each price level.
  std::size_t orders_per_level{3};

  /// Spacing between adjacent price levels, in ticks.
  core::PriceTicks tick_spacing{1};

  /// Size of each initial order, in lots.
  core::QuantityLots order_size{10};

  /// If non-empty, overrides the generated book with explicit orders.
  std::vector<InitialBookOrder> explicit_orders{};
};

// ---------------------------------------------------------------------------
// Simulation configuration
// ---------------------------------------------------------------------------

/// Complete configuration for a Hawkes-driven synthetic market simulation.
///
/// Components:
///   hawkes:        Hawkes intensity parameters (must have kEventTypeCount dims)
///   seed:          RNG seed for both the Hawkes thinning and mark distributions
///   horizon_seconds: simulation end time in seconds
///   marks:         mark distribution configuration (price, size, cancel rules)
///   initial_book:  initial book construction configuration
///
/// Illustrative only — not a calibrated model unless explicitly stated.
struct SyntheticMarketConfig final {
  HawkesParameters hawkes{};
  std::uint64_t seed{};
  double horizon_seconds{};
  MarkDistributionConfig marks{};
  InitialBookConfig initial_book{};
};

// ---------------------------------------------------------------------------
// Event log records
// ---------------------------------------------------------------------------

/// Outcome of a single exchange action attempted by the adapter.
enum class ActionOutcome : std::uint8_t {
  Accepted,                 ///< Exchange accepted the action.
  Rejected,                 ///< Exchange rejected (e.g. invalid order).
  CancellationInapplicable, ///< No eligible order to cancel on this side.
};

/// One record in the simulation event log.
///
/// Every Hawkes event produces exactly one record. The record documents
/// what the adapter attempted and what the exchange reported.
struct MarketEventRecord final {
  /// Hawkes event time, in seconds from simulation origin.
  double time_seconds{};

  /// The Hawkes event category that fired.
  EventType type{};

  /// Outcome of the exchange action.
  ActionOutcome outcome{ActionOutcome::Rejected};

  /// The order ID used (assigned by the adapter for new orders, or the
  /// target for cancellations).
  core::OrderId order_id{};

  /// Limit price for limit orders, 0 for market orders and cancellations.
  core::PriceTicks price_ticks{};

  /// Quantity for new orders; remaining quantity for cancelled orders.
  core::QuantityLots quantity{};

  /// Number of trades generated by this action.
  std::size_t trades_generated{};

  /// Pre-event Hawkes intensities at the time of this event.
  std::vector<double> pre_event_intensities{};
};

/// Aggregate statistics computed from the event log.
///
/// Invariant: hawkes_events == actions_accepted + actions_rejected +
///            cancellations_inapplicable
struct SyntheticMarketSummary final {
  /// Total events generated by the Hawkes process within the horizon.
  std::size_t hawkes_events{};

  /// Actions accepted by the exchange.
  std::size_t actions_accepted{};

  /// Actions rejected by the exchange (invalid orders).
  std::size_t actions_rejected{};

  /// Cancellation events where no eligible order existed on the relevant side.
  std::size_t cancellations_inapplicable{};

  /// Total trades executed.
  std::size_t trades{};

  /// Total traded quantity in lots.
  core::QuantityLots traded_quantity{};

  /// Count per event type.
  std::size_t events_by_type[kEventTypeCount]{};

  /// Final live order count in the book.
  std::size_t final_live_orders{};

  /// Final best bid (nullopt if bid side empty).
  std::optional<core::PriceTicks> final_best_bid{};

  /// Final best ask (nullopt if ask side empty).
  std::optional<core::PriceTicks> final_best_ask{};
};

/// Complete result of a synthetic market simulation.
struct SyntheticMarketResult final {
  /// Per-event log. One entry per Hawkes event.
  std::vector<MarketEventRecord> events{};

  /// All trades executed during the simulation.
  std::vector<exchange::Trade> trades{};

  /// Book snapshots taken at each event boundary (and at t=0 for initial book).
  std::vector<exchange::BookSnapshot> book_snapshots{};

  /// Aggregate statistics.
  SyntheticMarketSummary summary{};

  /// The configuration used (for reproducibility).
  SyntheticMarketConfig config{};

  /// NDJSON serialization of the event log.
  [[nodiscard]] std::string event_log_ndjson() const;

  /// NDJSON serialization of exchange fills produced by the simulation.
  [[nodiscard]] std::string trades_ndjson() const;

  /// Machine-readable record of the exact simulation configuration and output.
  [[nodiscard]] std::string run_manifest_json() const;

  /// Human-readable summary report.
  [[nodiscard]] std::string summary_report() const;
};

// ---------------------------------------------------------------------------
// Adapter
// ---------------------------------------------------------------------------

/// Adapter that maps Hawkes event types to exchange actions.
///
/// The adapter:
///   1. Validates the configuration.
///   2. Constructs the initial book through the matching engine.
///   3. Runs the Hawkes process to generate event times and types.
///   4. For each event, generates concrete order parameters using the
///      mark distribution config and the current book state.
///   5. Submits the action to the matching engine.
///   6. Records the outcome.
///
/// The exchange is a deterministic component: the adapter does all stochastic
/// work, then submits explicit, complete commands.
///
/// Timing: Hawkes event times are in seconds (double). They are converted to
/// nanosecond timestamps for the exchange using floor(time_seconds * 1e9).
/// Initial book orders are submitted at timestamp 0. Hawkes events start
/// at time > 0.
class HawkesMarketAdapter final {
 public:
  /// Run a complete simulation with the given configuration.
  /// The configuration is validated before any state is created.
  [[nodiscard]] SyntheticMarketResult run(const SyntheticMarketConfig& config);

  /// Generate the initial book orders from the configuration.
  /// This is exposed for testing.
  [[nodiscard]] static std::vector<InitialBookOrder> generate_initial_book(
      const InitialBookConfig& config);

 private:
  /// Validate the complete configuration. Throws std::invalid_argument
  /// if any component is inconsistent.
  static void validate(const SyntheticMarketConfig& config);

  /// Select a cancellation target on the given side.
  /// Returns nullopt if no eligible order exists.
  [[nodiscard]] static std::optional<core::OrderId> select_cancel_target(
      const exchange::OrderBook& book,
      core::Side side,
      CancelTargetPolicy policy,
      std::mt19937_64& rng);
};

}  // namespace lob::stochastic
