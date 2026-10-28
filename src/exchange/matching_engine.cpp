#include "lob/exchange/matching_engine.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lob::exchange {
namespace {
[[nodiscard]] bool crosses(const Order& incoming, const core::PriceTicks price) noexcept {
  return incoming.side == core::Side::Buy ? incoming.price_ticks >= price : incoming.price_ticks <= price;
}
}  // namespace

SubmissionResult MatchingEngine::submit_limit(const core::OrderId id, const core::TimestampNs time, const core::Side side, const core::PriceTicks price, const core::QuantityLots quantity) {
  return submit(Order{.id=id, .timestamp_ns=time, .side=side, .type=OrderType::Limit, .price_ticks=price, .initial_quantity=quantity, .remaining_quantity=quantity, .status=OrderStatus::New});
}
SubmissionResult MatchingEngine::submit_market(const core::OrderId id, const core::TimestampNs time, const core::Side side, const core::QuantityLots quantity) {
  return submit(Order{.id=id, .timestamp_ns=time, .side=side, .type=OrderType::Market, .price_ticks=0, .initial_quantity=quantity, .remaining_quantity=quantity, .status=OrderStatus::New});
}

RejectionReason MatchingEngine::validate(const Order& order) const noexcept {
  if (order.id == 0U) return RejectionReason::InvalidOrderId;
  if (accepted_ids_.contains(order.id)) return RejectionReason::DuplicateOrderId;
  if (!core::is_valid_timestamp(order.timestamp_ns)) return RejectionReason::InvalidTimestamp;
  if (!core::is_valid_side(order.side)) return RejectionReason::InvalidSide;
  if (!core::is_valid_quantity(order.initial_quantity)) return RejectionReason::InvalidQuantity;
  if (order.type == OrderType::Limit && order.price_ticks <= 0) return RejectionReason::InvalidPrice;
  return RejectionReason::None;
}

SubmissionResult MatchingEngine::submit(Order incoming) {
  SubmissionResult result{.order=incoming};
  result.rejection = validate(incoming);
  if (result.rejection != RejectionReason::None) {
    result.order.status = OrderStatus::Rejected;
    record_submission(result);
    return result;
  }
  accepted_ids_.insert(incoming.id);
  result.accepted = true;
  match(incoming, result.trades);
  if (incoming.remaining_quantity == 0U) incoming.status = OrderStatus::Filled;
  else if (incoming.type == OrderType::Market) { result.expired_market_quantity = incoming.remaining_quantity; incoming.status = OrderStatus::Expired; }
  else { book_.insert_limit(incoming); incoming.status = OrderStatus::Resting; }
  result.order = incoming;
  record_submission(result);
  return result;
}

void MatchingEngine::match(Order& incoming, std::vector<Trade>& action_trades) {
  while (incoming.remaining_quantity != 0U) {
    const auto best = incoming.side == core::Side::Buy ? book_.best_ask() : book_.best_bid();
    if (!best.has_value() || (incoming.type == OrderType::Limit && !crosses(incoming, *best))) return;
    const auto resting_id =
        book_.front_order_id_at(core::opposite(incoming.side), *best);
    if (!resting_id.has_value()) {
      throw std::logic_error{"best price has empty queue"};
    }
    const Order* resting = book_.find_order(*resting_id);
    if (resting == nullptr) throw std::logic_error{"queue references missing order"};
    const auto quantity = std::min(incoming.remaining_quantity, resting->remaining_quantity);
    const auto after = book_.apply_execution(resting->id, quantity);
    incoming.remaining_quantity -= quantity;
    if (next_trade_id_ == 0U) {
      throw std::overflow_error{"trade ID space exhausted"};
    }
    const core::TradeId trade_id = next_trade_id_;
    next_trade_id_ =
        trade_id == std::numeric_limits<core::TradeId>::max() ? 0U
                                                              : trade_id + 1U;
    Trade trade{.id=trade_id, .timestamp_ns=incoming.timestamp_ns, .price_ticks=*best, .quantity=quantity, .aggressor_side=incoming.side, .resting_order_id=after.id, .incoming_order_id=incoming.id, .resting_remaining_quantity=after.remaining_quantity, .incoming_remaining_quantity=incoming.remaining_quantity};
    trades_.push_back(trade); action_trades.push_back(trade);
    events_.push_back(ExchangeEvent{.type=ExchangeEventType::Execution, .timestamp_ns=trade.timestamp_ns, .order_id=incoming.id, .accepted=true, .resulting_status=after.status, .remaining_quantity=incoming.remaining_quantity, .trade=trade});
  }
}

void MatchingEngine::record_submission(const SubmissionResult& result) {
  events_.push_back(ExchangeEvent{.type=ExchangeEventType::Submission, .timestamp_ns=result.order.timestamp_ns, .order_id=result.order.id, .accepted=result.accepted, .rejection=result.rejection, .resulting_status=result.order.status, .remaining_quantity=result.order.remaining_quantity});
}

CancellationResult MatchingEngine::cancel(const core::OrderId id, const core::TimestampNs time) {
  CancellationResult result{.timestamp_ns=time, .order_id=id};
  if (!core::is_valid_timestamp(time)) result.rejection=RejectionReason::InvalidTimestamp;
  else if (id == 0U) result.rejection=RejectionReason::InvalidOrderId;
  else { result.cancelled_order=book_.remove(id); if (result.cancelled_order) result.accepted=true; else result.rejection=RejectionReason::UnknownOrderId; }
  events_.push_back(ExchangeEvent{.type=ExchangeEventType::Cancellation, .timestamp_ns=time, .order_id=id, .accepted=result.accepted, .rejection=result.rejection, .resulting_status=result.cancelled_order ? result.cancelled_order->status : OrderStatus::Rejected, .remaining_quantity=result.cancelled_order ? result.cancelled_order->remaining_quantity : 0U});
  return result;
}

#if defined(LOB_ENABLE_TESTING)
void MatchingEngine::assert_invariants_for_testing() const {
  book_.assert_invariants_for_testing();
  if (next_trade_id_ != static_cast<core::TradeId>(trades_.size()) + 1U) throw std::logic_error{"noncontiguous trade IDs"};
}
#endif
}  // namespace lob::exchange
