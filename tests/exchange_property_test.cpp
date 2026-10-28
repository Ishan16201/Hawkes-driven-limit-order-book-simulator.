#include "lob/exchange/matching_engine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace {

using lob::core::OrderId;
using lob::core::PriceTicks;
using lob::core::QuantityLots;
using lob::core::Side;
using lob::exchange::MatchingEngine;
using lob::exchange::Trade;

struct ReferenceOrder final {
  OrderId id{};
  Side side{};
  PriceTicks price{};
  QuantityLots remaining{};
  std::uint64_t priority{};
};

struct ReferenceBook final {
  std::vector<ReferenceOrder> orders;
  std::vector<Trade> trades;
  std::uint64_t next_priority{};

  [[nodiscard]] std::vector<Trade> submit(const OrderId id,
                                          const std::int64_t time,
                                          const Side side,
                                          const PriceTicks price,
                                          QuantityLots quantity,
                                          const bool market) {
    std::vector<Trade> action_trades;
    while (quantity > 0U) {
      std::size_t best = orders.size();
      for (std::size_t i = 0; i < orders.size(); ++i) {
        const auto& candidate = orders[i];
        if (candidate.side == side ||
            (!market &&
             (side == Side::Buy ? price < candidate.price
                                : price > candidate.price))) {
          continue;
        }
        if (best == orders.size() ||
            (side == Side::Buy
                 ? candidate.price < orders[best].price
                 : candidate.price > orders[best].price) ||
            (candidate.price == orders[best].price &&
             candidate.priority < orders[best].priority)) {
          best = i;
        }
      }
      if (best == orders.size()) {
        break;
      }

      auto& resting = orders[best];
      const QuantityLots filled = std::min(quantity, resting.remaining);
      quantity -= filled;
      resting.remaining -= filled;
      Trade trade{.id = static_cast<lob::core::TradeId>(trades.size() + 1U),
                  .timestamp_ns = time,
                  .price_ticks = resting.price,
                  .quantity = filled,
                  .aggressor_side = side,
                  .resting_order_id = resting.id,
                  .incoming_order_id = id,
                  .resting_remaining_quantity = resting.remaining,
                  .incoming_remaining_quantity = quantity};
      trades.push_back(trade);
      action_trades.push_back(trade);
      if (resting.remaining == 0U) {
        orders.erase(orders.begin() + static_cast<std::ptrdiff_t>(best));
      }
    }

    if (!market && quantity > 0U) {
      orders.push_back(
          ReferenceOrder{id, side, price, quantity, next_priority++});
    }
    return action_trades;
  }

  [[nodiscard]] bool cancel(const OrderId id) {
    const auto found = std::find_if(
        orders.begin(), orders.end(),
        [id](const ReferenceOrder& order) { return order.id == id; });
    if (found == orders.end()) {
      return false;
    }
    orders.erase(found);
    return true;
  }
};

void compare_state(const MatchingEngine& engine, const ReferenceBook& model) {
  engine.assert_invariants_for_testing();
  REQUIRE(engine.book().live_order_count() == model.orders.size());

  std::optional<PriceTicks> best_bid;
  std::optional<PriceTicks> best_ask;
  for (const auto& order : model.orders) {
    if (order.side == Side::Buy) {
      best_bid = best_bid ? std::max(*best_bid, order.price) : order.price;
    } else {
      best_ask = best_ask ? std::min(*best_ask, order.price) : order.price;
    }
    const auto* indexed = engine.book().find_order(order.id);
    REQUIRE(indexed != nullptr);
    REQUIRE(indexed->side == order.side);
    REQUIRE(indexed->price_ticks == order.price);
    REQUIRE(indexed->remaining_quantity == order.remaining);
  }
  REQUIRE(engine.book().best_bid() == best_bid);
  REQUIRE(engine.book().best_ask() == best_ask);

  for (const Side side : {Side::Buy, Side::Sell}) {
    for (PriceTicks price = 94; price <= 106; ++price) {
      std::vector<OrderId> expected_ids;
      QuantityLots expected_quantity = 0U;
      for (const auto& order : model.orders) {
        if (order.side == side && order.price == price) {
          expected_ids.push_back(order.id);
          expected_quantity += order.remaining;
        }
      }
      const auto actual_quantity =
          engine.book().displayed_quantity_at(side, price);
      if (expected_ids.empty()) {
        REQUIRE_FALSE(actual_quantity.has_value());
      } else {
        REQUIRE(actual_quantity == expected_quantity);
      }
      REQUIRE(engine.book().order_ids_at(side, price) == expected_ids);
    }
  }

  REQUIRE(engine.trades().size() == model.trades.size());
  for (std::size_t i = 0; i < model.trades.size(); ++i) {
    REQUIRE(engine.trades()[i] == model.trades[i]);
  }
}

}  // namespace

TEST_CASE("randomized matching sequences agree with an independent reference book") {
  for (const std::uint64_t seed : {1U, 7U, 42U, 20261028U}) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> action_draw(0, 99);
    std::uniform_int_distribution<int> side_draw(0, 1);
    std::uniform_int_distribution<int> price_draw(95, 105);
    std::uniform_int_distribution<int> quantity_draw(1, 8);

    MatchingEngine engine;
    ReferenceBook model;
    for (std::uint64_t sequence = 1U; sequence <= 600U; ++sequence) {
      const int action = action_draw(rng);
      const Side side = side_draw(rng) == 0 ? Side::Buy : Side::Sell;
      const auto id = static_cast<OrderId>(sequence);
      const auto timestamp = static_cast<lob::core::TimestampNs>(sequence);
      if (action < 65) {
        const PriceTicks price = price_draw(rng);
        const QuantityLots quantity =
            static_cast<QuantityLots>(quantity_draw(rng));
        const auto expected =
            model.submit(id, timestamp, side, price, quantity, false);
        const auto actual =
            engine.submit_limit(id, timestamp, side, price, quantity);
        REQUIRE(actual.accepted);
        REQUIRE(actual.trades == expected);
      } else if (action < 85) {
        const QuantityLots quantity =
            static_cast<QuantityLots>(quantity_draw(rng));
        const auto expected =
            model.submit(id, timestamp, side, 0, quantity, true);
        const auto actual =
            engine.submit_market(id, timestamp, side, quantity);
        REQUIRE(actual.accepted);
        REQUIRE(actual.trades == expected);
      } else {
        std::uniform_int_distribution<OrderId> id_draw(0U, sequence + 3U);
        const OrderId target = id_draw(rng);
        const bool expected = model.cancel(target);
        const auto actual = engine.cancel(target, timestamp);
        REQUIRE(actual.accepted == expected);
      }
      compare_state(engine, model);
    }
  }
}

TEST_CASE("price-level quantity overflow leaves the order book unchanged") {
  lob::exchange::OrderBook book;
  book.insert_limit({.id = 1U,
                     .timestamp_ns = 0,
                     .side = Side::Buy,
                     .type = lob::exchange::OrderType::Limit,
                     .price_ticks = 100,
                     .initial_quantity =
                         std::numeric_limits<QuantityLots>::max(),
                     .remaining_quantity =
                         std::numeric_limits<QuantityLots>::max(),
                     .status = lob::exchange::OrderStatus::New});

  REQUIRE_THROWS_AS(
      book.insert_limit({.id = 2U,
                         .timestamp_ns = 1,
                         .side = Side::Buy,
                         .type = lob::exchange::OrderType::Limit,
                         .price_ticks = 100,
                         .initial_quantity = 1U,
                         .remaining_quantity = 1U,
                         .status = lob::exchange::OrderStatus::New}),
      std::overflow_error);
  REQUIRE(book.live_order_count() == 1U);
  REQUIRE(book.displayed_quantity_at(Side::Buy, 100) ==
          std::numeric_limits<QuantityLots>::max());
  REQUIRE_FALSE(book.contains(2U));
  REQUIRE_NOTHROW(book.assert_invariants_for_testing());
}
