#pragma once

#include "lob/exchange/order_book.hpp"

#include <optional>
#include <unordered_set>
#include <vector>

namespace lob::exchange {

enum class RejectionReason : std::uint8_t { None, InvalidOrderId, DuplicateOrderId, InvalidTimestamp, InvalidSide, InvalidPrice, InvalidQuantity, UnknownOrderId };

struct Trade final {
  core::TradeId id{};
  core::TimestampNs timestamp_ns{};
  core::PriceTicks price_ticks{};
  core::QuantityLots quantity{};
  core::Side aggressor_side{core::Side::Buy};
  core::OrderId resting_order_id{};
  core::OrderId incoming_order_id{};
  core::QuantityLots resting_remaining_quantity{};
  core::QuantityLots incoming_remaining_quantity{};
  [[nodiscard]] bool operator==(const Trade&) const = default;
};

enum class ExchangeEventType : std::uint8_t { Submission, Execution, Cancellation };
struct ExchangeEvent final {
  ExchangeEventType type{};
  core::TimestampNs timestamp_ns{};
  core::OrderId order_id{};
  bool accepted{};
  RejectionReason rejection{RejectionReason::None};
  OrderStatus resulting_status{OrderStatus::Rejected};
  core::QuantityLots remaining_quantity{};
  std::optional<Trade> trade{};
};

struct SubmissionResult final {
  bool accepted{};
  RejectionReason rejection{RejectionReason::None};
  Order order{};
  core::QuantityLots expired_market_quantity{};
  std::vector<Trade> trades{};
};
struct CancellationResult final {
  bool accepted{};
  RejectionReason rejection{RejectionReason::None};
  core::TimestampNs timestamp_ns{};
  core::OrderId order_id{};
  std::optional<Order> cancelled_order{};
};

/// Single-instrument deterministic matching. Invocation order is event order;
/// unfilled market quantity expires immediately and never rests.
class MatchingEngine final {
 public:
  [[nodiscard]] SubmissionResult submit_limit(core::OrderId id, core::TimestampNs timestamp_ns, core::Side side, core::PriceTicks price_ticks, core::QuantityLots quantity);
  [[nodiscard]] SubmissionResult submit_market(core::OrderId id, core::TimestampNs timestamp_ns, core::Side side, core::QuantityLots quantity);
  [[nodiscard]] CancellationResult cancel(core::OrderId id, core::TimestampNs timestamp_ns);
  [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
  [[nodiscard]] const std::vector<Trade>& trades() const noexcept { return trades_; }
  [[nodiscard]] const std::vector<ExchangeEvent>& events() const noexcept { return events_; }
  [[nodiscard]] BookSnapshot snapshot(
      core::TimestampNs timestamp_ns, std::size_t max_levels = 0) const {
    return book_.snapshot(timestamp_ns, max_levels);
  }
#if defined(LOB_ENABLE_TESTING)
  void assert_invariants_for_testing() const;
#endif
 private:
  [[nodiscard]] SubmissionResult submit(Order incoming);
  [[nodiscard]] RejectionReason validate(const Order& order) const noexcept;
  void match(Order& incoming, std::vector<Trade>& action_trades);
  void record_submission(const SubmissionResult& result);
  OrderBook book_;
  std::unordered_set<core::OrderId> accepted_ids_;
  core::TradeId next_trade_id_{1U};
  std::vector<Trade> trades_;
  std::vector<ExchangeEvent> events_;
};

}  // namespace lob::exchange
