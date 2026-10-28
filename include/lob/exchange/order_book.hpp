#pragma once

#include "lob/core/types.hpp"

#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lob::exchange {

enum class OrderType : std::uint8_t { Limit, Market };

enum class OrderStatus : std::uint8_t {
  New,
  Resting,
  PartiallyFilled,
  Filled,
  Cancelled,
  Expired,
  Rejected,
};

[[nodiscard]] constexpr bool is_live(const OrderStatus status) noexcept {
  return status == OrderStatus::Resting || status == OrderStatus::PartiallyFilled;
}

/// A value record. A Market order has no resting price and cannot be inserted by
/// this storage-only book; matching will handle it in a later module.
struct Order final {
  core::OrderId id{};
  core::TimestampNs timestamp_ns{};
  core::Side side{core::Side::Buy};
  OrderType type{OrderType::Limit};
  core::PriceTicks price_ticks{};
  core::QuantityLots initial_quantity{};
  core::QuantityLots remaining_quantity{};
  OrderStatus status{OrderStatus::New};
};

/// Orders are FIFO by their position in order_ids. displayed_quantity must equal
/// the sum of remaining quantities of those indexed orders.
struct PriceLevel final {
  using OrderQueue = std::list<core::OrderId>;

  core::PriceTicks price_ticks{};
  core::QuantityLots displayed_quantity{};
  OrderQueue order_ids{};
};

struct LevelQuote final {
  core::PriceTicks price_ticks{};
  core::QuantityLots quantity{};
  [[nodiscard]] bool operator==(const LevelQuote&) const = default;
};

struct BookSnapshot final {
  core::TimestampNs timestamp_ns{};
  std::vector<LevelQuote> bids{};
  std::vector<LevelQuote> asks{};
  [[nodiscard]] bool operator==(const BookSnapshot&) const = default;
};

class OrderBook final {
 public:
  using BidLevels = std::map<core::PriceTicks, PriceLevel, std::greater<core::PriceTicks>>;
  using AskLevels = std::map<core::PriceTicks, PriceLevel, std::less<core::PriceTicks>>;

  OrderBook() = default;
  ~OrderBook() = default;
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) = delete;
  OrderBook& operator=(OrderBook&&) = delete;

  /// Inserts a new resting limit order. Throws std::invalid_argument for invalid
  /// input and duplicate IDs. Ownership transfers into the book on success.
  void insert_limit(Order order);

  /// Removes a live order and returns its terminal Cancelled record. A zero ID is
  /// invalid and throws; an unknown nonzero ID returns std::nullopt.
  [[nodiscard]] std::optional<Order> remove(core::OrderId id);

  /// Partial execution preserves FIFO position; full execution removes the
  /// queue/index entry and returns a Filled terminal record.
  [[nodiscard]] Order apply_execution(core::OrderId id, core::QuantityLots quantity);

  /// Returns a pointer owned by this book, or nullptr for an unknown/zero ID.
  /// The pointer is invalidated only by removal of that specific order or book
  /// destruction; callers must not retain it across those operations.
  [[nodiscard]] const Order* find_order(core::OrderId id) const noexcept;
  [[nodiscard]] bool contains(core::OrderId id) const noexcept;

  [[nodiscard]] const BidLevels& bids() const noexcept { return bids_; }
  [[nodiscard]] const AskLevels& asks() const noexcept { return asks_; }

  [[nodiscard]] std::optional<core::PriceTicks> best_bid() const noexcept;
  [[nodiscard]] std::optional<core::PriceTicks> best_ask() const noexcept;
  [[nodiscard]] std::optional<core::QuantityLots> displayed_quantity_at(
      core::Side side, core::PriceTicks price_ticks) const noexcept;
  [[nodiscard]] std::vector<core::OrderId> order_ids_at(
      core::Side side, core::PriceTicks price_ticks) const;
  [[nodiscard]] std::optional<core::OrderId> front_order_id_at(
      core::Side side, core::PriceTicks price_ticks) const noexcept;
  [[nodiscard]] std::size_t live_order_count() const noexcept;
  [[nodiscard]] std::size_t price_level_count(core::Side side) const noexcept;

  /// Returns a snapshot of price levels up to max_levels (0 = all levels).
  [[nodiscard]] BookSnapshot snapshot(
      core::TimestampNs timestamp_ns, std::size_t max_levels = 0) const;

#if defined(LOB_ENABLE_TESTING)
  /// Test-only full consistency verification. Throws std::logic_error on any
  /// index, queue, aggregate, side, or lifecycle inconsistency.
  void assert_invariants_for_testing() const;
#endif

 private:
  struct IndexedOrder final {
    Order order;
    PriceLevel::OrderQueue::iterator queue_position;
  };

  [[nodiscard]] static bool is_valid_resting_order(const Order& order) noexcept;
  void insert_into_bids(Order order);
  void insert_into_asks(Order order);

  BidLevels bids_;
  AskLevels asks_;
  std::unordered_map<core::OrderId, IndexedOrder> order_index_;
};

}  // namespace lob::exchange
