#include "lob/exchange/order_book.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

namespace {

lob::exchange::Order limit_order(const lob::core::OrderId id, const lob::core::Side side,
                                 const lob::core::PriceTicks price,
                                 const lob::core::QuantityLots quantity,
                                 const lob::core::TimestampNs timestamp = 0) {
  return {.id = id,
          .timestamp_ns = timestamp,
          .side = side,
          .type = lob::exchange::OrderType::Limit,
          .price_ticks = price,
          .initial_quantity = quantity,
          .remaining_quantity = quantity,
          .status = lob::exchange::OrderStatus::New};
}

}  // namespace

TEST_CASE("an empty book has no best prices or indexed orders") {
  lob::exchange::OrderBook book;
  REQUIRE_FALSE(book.best_bid().has_value());
  REQUIRE_FALSE(book.best_ask().has_value());
  REQUIRE_FALSE(book.find_order(1U));
  REQUIRE(book.live_order_count() == 0U);
  REQUIRE(book.price_level_count(lob::core::Side::Buy) == 0U);
  REQUIRE(book.price_level_count(lob::core::Side::Sell) == 0U);
  REQUIRE_NOTHROW(book.assert_invariants_for_testing());
}

TEST_CASE("orders at a price level preserve FIFO and aggregate displayed quantity") {
  lob::exchange::OrderBook book;
  book.insert_limit(limit_order(1U, lob::core::Side::Buy, 100, 4U));
  book.insert_limit(limit_order(2U, lob::core::Side::Buy, 100, 7U));

  REQUIRE(book.best_bid() == 100);
  REQUIRE(book.displayed_quantity_at(lob::core::Side::Buy, 100) == 11U);
  REQUIRE(book.order_ids_at(lob::core::Side::Buy, 100) == std::vector<lob::core::OrderId>{1U, 2U});
  REQUIRE(book.front_order_id_at(lob::core::Side::Buy, 100) == 1U);
  REQUIRE(book.find_order(2U)->status == lob::exchange::OrderStatus::Resting);
  REQUIRE_NOTHROW(book.assert_invariants_for_testing());
}

TEST_CASE("best prices follow descending bids and ascending asks") {
  lob::exchange::OrderBook book;
  book.insert_limit(limit_order(1U, lob::core::Side::Buy, 99, 2U));
  book.insert_limit(limit_order(2U, lob::core::Side::Buy, 103, 3U));
  book.insert_limit(limit_order(3U, lob::core::Side::Sell, 105, 5U));
  book.insert_limit(limit_order(4U, lob::core::Side::Sell, 101, 7U));

  REQUIRE(book.best_bid() == 103);
  REQUIRE(book.best_ask() == 101);
  REQUIRE(book.displayed_quantity_at(lob::core::Side::Buy, 99) == 2U);
  REQUIRE(book.displayed_quantity_at(lob::core::Side::Sell, 105) == 5U);
  REQUIRE(book.price_level_count(lob::core::Side::Buy) == 2U);
  REQUIRE(book.price_level_count(lob::core::Side::Sell) == 2U);
  REQUIRE_NOTHROW(book.assert_invariants_for_testing());
}

TEST_CASE("removal updates the order index, level aggregate, and empty levels") {
  lob::exchange::OrderBook book;
  book.insert_limit(limit_order(1U, lob::core::Side::Sell, 200, 3U));
  book.insert_limit(limit_order(2U, lob::core::Side::Sell, 200, 5U));

  const auto first = book.remove(1U);
  REQUIRE(first.has_value());
  REQUIRE(first->status == lob::exchange::OrderStatus::Cancelled);
  REQUIRE_FALSE(book.contains(1U));
  REQUIRE(book.displayed_quantity_at(lob::core::Side::Sell, 200) == 5U);
  REQUIRE(book.order_ids_at(lob::core::Side::Sell, 200) == std::vector<lob::core::OrderId>{2U});
  REQUIRE_NOTHROW(book.assert_invariants_for_testing());

  REQUIRE(book.remove(2U).has_value());
  REQUIRE_FALSE(book.displayed_quantity_at(lob::core::Side::Sell, 200).has_value());
  REQUIRE_FALSE(book.best_ask().has_value());
  REQUIRE(book.price_level_count(lob::core::Side::Sell) == 0U);
  REQUIRE_FALSE(book.remove(2U).has_value());
  REQUIRE_NOTHROW(book.assert_invariants_for_testing());
}

TEST_CASE("duplicate IDs and invalid order inputs are explicit failures") {
  lob::exchange::OrderBook book;
  book.insert_limit(limit_order(1U, lob::core::Side::Buy, 100, 1U));
  REQUIRE_THROWS_AS(book.insert_limit(limit_order(1U, lob::core::Side::Sell, 101, 1U)),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(book.insert_limit(limit_order(0U, lob::core::Side::Buy, 100, 1U)),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(book.insert_limit(limit_order(2U, lob::core::Side::Buy, 0, 1U)),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(book.insert_limit(limit_order(2U, lob::core::Side::Buy, 100, 0U)),
                    std::invalid_argument);
  const auto invalid_side = limit_order(2U, static_cast<lob::core::Side>(99U), 100, 1U);
  REQUIRE_THROWS_AS(book.insert_limit(invalid_side), std::invalid_argument);
  REQUIRE_THROWS_AS(book.remove(0U), std::invalid_argument);

  auto market = limit_order(3U, lob::core::Side::Buy, 100, 1U);
  market.type = lob::exchange::OrderType::Market;
  REQUIRE_THROWS_AS(book.insert_limit(market), std::invalid_argument);
  REQUIRE_NOTHROW(book.assert_invariants_for_testing());
}
