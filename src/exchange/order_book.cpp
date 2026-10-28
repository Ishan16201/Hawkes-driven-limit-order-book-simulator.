#include "lob/exchange/order_book.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace lob::exchange {
namespace {

void require_valid_identifier(const core::OrderId id) {
  if (id == 0U) {
    throw std::invalid_argument{"order ID zero is invalid"};
  }
}

template <typename Levels>
std::optional<core::QuantityLots> displayed_quantity_at_level(
    const Levels& levels, const core::PriceTicks price_ticks) noexcept {
  const auto iterator = levels.find(price_ticks);
  if (iterator == levels.end()) {
    return std::nullopt;
  }
  return iterator->second.displayed_quantity;
}

template <typename Levels>
std::vector<core::OrderId> order_ids_at_level(const Levels& levels,
                                               const core::PriceTicks price_ticks) {
  const auto iterator = levels.find(price_ticks);
  if (iterator == levels.end()) {
    return {};
  }
  return {iterator->second.order_ids.begin(), iterator->second.order_ids.end()};
}

}  // namespace

bool OrderBook::is_valid_resting_order(const Order& order) noexcept {
  return order.id != 0U && core::is_valid_timestamp(order.timestamp_ns) &&
         core::is_valid_side(order.side) && order.type == OrderType::Limit && order.price_ticks > 0 &&
         core::is_valid_quantity(order.initial_quantity) &&
         core::is_valid_quantity(order.remaining_quantity) &&
         order.remaining_quantity <= order.initial_quantity && order.status == OrderStatus::New;
}

void OrderBook::insert_limit(Order order) {
  if (!is_valid_resting_order(order)) {
    throw std::invalid_argument{"invalid new resting limit order"};
  }
  if (contains(order.id)) {
    throw std::invalid_argument{"duplicate live order ID"};
  }

  order.status = OrderStatus::Resting;
  if (order.side == core::Side::Buy) {
    insert_into_bids(std::move(order));
  } else {
    insert_into_asks(std::move(order));
  }
}

void OrderBook::insert_into_bids(Order order) {
  const auto [level_iterator, inserted_level] =
      bids_.try_emplace(order.price_ticks, PriceLevel{.price_ticks = order.price_ticks});
  PriceLevel& level = level_iterator->second;
  if (std::numeric_limits<core::QuantityLots>::max() - level.displayed_quantity <
      order.remaining_quantity) {
    if (inserted_level) {
      bids_.erase(level_iterator);
    }
    throw std::overflow_error{"price-level aggregate quantity overflow"};
  }
  level.order_ids.push_back(order.id);
  const auto queue_position = std::prev(level.order_ids.end());
  try {
    order_index_.emplace(
        order.id, IndexedOrder{.order = std::move(order), .queue_position = queue_position});
  } catch (...) {
    level.order_ids.erase(queue_position);
    if (level.order_ids.empty()) {
      bids_.erase(level_iterator);
    }
    throw;
  }
  level.displayed_quantity += order_index_.at(*queue_position).order.remaining_quantity;
}

void OrderBook::insert_into_asks(Order order) {
  const auto [level_iterator, inserted_level] =
      asks_.try_emplace(order.price_ticks, PriceLevel{.price_ticks = order.price_ticks});
  PriceLevel& level = level_iterator->second;
  if (std::numeric_limits<core::QuantityLots>::max() - level.displayed_quantity <
      order.remaining_quantity) {
    if (inserted_level) {
      asks_.erase(level_iterator);
    }
    throw std::overflow_error{"price-level aggregate quantity overflow"};
  }
  level.order_ids.push_back(order.id);
  const auto queue_position = std::prev(level.order_ids.end());
  try {
    order_index_.emplace(
        order.id, IndexedOrder{.order = std::move(order), .queue_position = queue_position});
  } catch (...) {
    level.order_ids.erase(queue_position);
    if (level.order_ids.empty()) {
      asks_.erase(level_iterator);
    }
    throw;
  }
  level.displayed_quantity += order_index_.at(*queue_position).order.remaining_quantity;
}

std::optional<Order> OrderBook::remove(const core::OrderId id) {
  require_valid_identifier(id);
  const auto index_iterator = order_index_.find(id);
  if (index_iterator == order_index_.end()) {
    return std::nullopt;
  }

  Order removed = index_iterator->second.order;
  if (removed.side == core::Side::Buy) {
    const auto level_iterator = bids_.find(removed.price_ticks);
    if (level_iterator == bids_.end()) {
      throw std::logic_error{"order index references a missing bid price level"};
    }
    PriceLevel& level = level_iterator->second;
    if (level.displayed_quantity < removed.remaining_quantity) {
      throw std::logic_error{"bid aggregate is below indexed order quantity"};
    }
    level.order_ids.erase(index_iterator->second.queue_position);
    level.displayed_quantity -= removed.remaining_quantity;
    if (level.order_ids.empty()) {
      bids_.erase(level_iterator);
    }
  } else {
    const auto level_iterator = asks_.find(removed.price_ticks);
    if (level_iterator == asks_.end()) {
      throw std::logic_error{"order index references a missing ask price level"};
    }
    PriceLevel& level = level_iterator->second;
    if (level.displayed_quantity < removed.remaining_quantity) {
      throw std::logic_error{"ask aggregate is below indexed order quantity"};
    }
    level.order_ids.erase(index_iterator->second.queue_position);
    level.displayed_quantity -= removed.remaining_quantity;
    if (level.order_ids.empty()) {
      asks_.erase(level_iterator);
    }
  }
  order_index_.erase(index_iterator);

  removed.status = OrderStatus::Cancelled;
  return removed;
}

Order OrderBook::apply_execution(const core::OrderId id, const core::QuantityLots quantity) {
  require_valid_identifier(id);
  core::require_valid_quantity(quantity);
  const auto index_iterator = order_index_.find(id);
  if (index_iterator == order_index_.end() || quantity > index_iterator->second.order.remaining_quantity) {
    throw std::invalid_argument{"invalid execution against live order"};
  }
  Order& live = index_iterator->second.order;
  const bool is_buy = live.side == core::Side::Buy;
  auto execute = [&](auto& levels) -> Order {
    const auto level_iterator = levels.find(live.price_ticks);
    if (level_iterator == levels.end() || level_iterator->second.displayed_quantity < quantity) {
      throw std::logic_error{"inconsistent order-book state during execution"};
    }
    PriceLevel& level = level_iterator->second;
    level.displayed_quantity -= quantity;
    live.remaining_quantity -= quantity;
    if (live.remaining_quantity != 0U) {
      live.status = OrderStatus::PartiallyFilled;
      return live;
    }
    Order filled = live;
    filled.status = OrderStatus::Filled;
    level.order_ids.erase(index_iterator->second.queue_position);
    order_index_.erase(index_iterator);
    if (level.order_ids.empty()) {
      levels.erase(level_iterator);
    }
    return filled;
  };
  return is_buy ? execute(bids_) : execute(asks_);
}

const Order* OrderBook::find_order(const core::OrderId id) const noexcept {
  if (id == 0U) {
    return nullptr;
  }
  const auto iterator = order_index_.find(id);
  return iterator == order_index_.end() ? nullptr : &iterator->second.order;
}

bool OrderBook::contains(const core::OrderId id) const noexcept {
  return find_order(id) != nullptr;
}

std::optional<core::PriceTicks> OrderBook::best_bid() const noexcept {
  if (bids_.empty()) {
    return std::nullopt;
  }
  return bids_.begin()->first;
}

std::optional<core::PriceTicks> OrderBook::best_ask() const noexcept {
  if (asks_.empty()) {
    return std::nullopt;
  }
  return asks_.begin()->first;
}

std::optional<core::QuantityLots> OrderBook::displayed_quantity_at(
    const core::Side side, const core::PriceTicks price_ticks) const noexcept {
  return side == core::Side::Buy ? displayed_quantity_at_level(bids_, price_ticks)
                                 : displayed_quantity_at_level(asks_, price_ticks);
}

std::vector<core::OrderId> OrderBook::order_ids_at(const core::Side side,
                                                    const core::PriceTicks price_ticks) const {
  return side == core::Side::Buy ? order_ids_at_level(bids_, price_ticks)
                                 : order_ids_at_level(asks_, price_ticks);
}

std::optional<core::OrderId> OrderBook::front_order_id_at(
    const core::Side side, const core::PriceTicks price_ticks) const noexcept {
  const auto find_front = [price_ticks](const auto& levels)
      -> std::optional<core::OrderId> {
    const auto level = levels.find(price_ticks);
    if (level == levels.end() || level->second.order_ids.empty()) {
      return std::nullopt;
    }
    return level->second.order_ids.front();
  };
  return side == core::Side::Buy ? find_front(bids_) : find_front(asks_);
}

std::size_t OrderBook::live_order_count() const noexcept {
  return order_index_.size();
}

std::size_t OrderBook::price_level_count(const core::Side side) const noexcept {
  return side == core::Side::Buy ? bids_.size() : asks_.size();
}

BookSnapshot OrderBook::snapshot(
    const core::TimestampNs timestamp_ns, const std::size_t max_levels) const {
  BookSnapshot result;
  result.timestamp_ns = timestamp_ns;

  std::size_t count = 0;
  for (const auto& [price, level] : bids_) {
    if (max_levels > 0 && count >= max_levels) {
      break;
    }
    result.bids.push_back(LevelQuote{
        .price_ticks = price,
        .quantity = level.displayed_quantity,
    });
    ++count;
  }

  count = 0;
  for (const auto& [price, level] : asks_) {
    if (max_levels > 0 && count >= max_levels) {
      break;
    }
    result.asks.push_back(LevelQuote{
        .price_ticks = price,
        .quantity = level.displayed_quantity,
    });
    ++count;
  }

  return result;
}

#if defined(LOB_ENABLE_TESTING)
void OrderBook::assert_invariants_for_testing() const {
  std::size_t orders_seen = 0U;
  const auto verify_side = [this, &orders_seen](const auto& levels, const core::Side expected_side) {
    for (const auto& [price_ticks, level] : levels) {
      if (price_ticks != level.price_ticks || price_ticks <= 0 || level.order_ids.empty()) {
        throw std::logic_error{"invalid price level"};
      }
      core::QuantityLots quantity_sum = 0U;
      for (const core::OrderId id : level.order_ids) {
        const auto index_iterator = order_index_.find(id);
        if (index_iterator == order_index_.end() || *index_iterator->second.queue_position != id) {
          throw std::logic_error{"queue references an absent or invalid indexed order"};
        }
        const Order& order = index_iterator->second.order;
        if (order.id != id || order.side != expected_side || order.price_ticks != price_ticks ||
            !is_live(order.status) || !core::is_valid_quantity(order.remaining_quantity) ||
            std::numeric_limits<core::QuantityLots>::max() - quantity_sum < order.remaining_quantity) {
          throw std::logic_error{"indexed order is inconsistent with its price level"};
        }
        quantity_sum += order.remaining_quantity;
        ++orders_seen;
      }
      if (quantity_sum != level.displayed_quantity) {
        throw std::logic_error{"price-level aggregate quantity is inconsistent"};
      }
    }
  };

  verify_side(bids_, core::Side::Buy);
  verify_side(asks_, core::Side::Sell);
  if (orders_seen != order_index_.size()) {
    throw std::logic_error{"order-ID index contains an order absent from price levels"};
  }
}
#endif

}  // namespace lob::exchange
