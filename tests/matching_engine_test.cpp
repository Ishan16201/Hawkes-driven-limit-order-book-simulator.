#include "lob/exchange/matching_engine.hpp"
#include <catch2/catch_test_macros.hpp>

namespace { void consistent(const lob::exchange::MatchingEngine& e) { REQUIRE_NOTHROW(e.assert_invariants_for_testing()); } }

TEST_CASE("limits rest without opposite liquidity") {
  lob::exchange::MatchingEngine e;
  REQUIRE(e.submit_limit(1, 1, lob::core::Side::Buy, 100, 5).order.status == lob::exchange::OrderStatus::Resting);
  REQUIRE(e.submit_limit(2, 2, lob::core::Side::Sell, 105, 4).order.status == lob::exchange::OrderStatus::Resting);
  REQUIRE(e.book().best_bid() == 100); REQUIRE(e.book().best_ask() == 105); consistent(e);
}
TEST_CASE("marketable buy and sell execute at resting price") {
  lob::exchange::MatchingEngine e;
  static_cast<void>(e.submit_limit(1, 1, lob::core::Side::Sell, 101, 5));
  const auto buy=e.submit_limit(2, 2, lob::core::Side::Buy, 102, 3);
  REQUIRE(buy.order.status == lob::exchange::OrderStatus::Filled); REQUIRE(buy.trades[0].price_ticks == 101); REQUIRE(e.book().find_order(1)->remaining_quantity == 2);
  static_cast<void>(e.submit_limit(3, 3, lob::core::Side::Buy, 100, 3));
  const auto sell=e.submit_limit(4, 4, lob::core::Side::Sell, 99, 5);
  REQUIRE(sell.trades[0].price_ticks == 100); REQUIRE(sell.order.status == lob::exchange::OrderStatus::Resting); REQUIRE(sell.order.remaining_quantity == 2); consistent(e);
}
TEST_CASE("hand-calculated multi-level FIFO sequence updates best price") {
  lob::exchange::MatchingEngine e;
  static_cast<void>(e.submit_limit(1,1,lob::core::Side::Sell,101,2)); static_cast<void>(e.submit_limit(2,2,lob::core::Side::Sell,101,3)); static_cast<void>(e.submit_limit(3,3,lob::core::Side::Sell,102,4));
  const auto r=e.submit_limit(4,4,lob::core::Side::Buy,102,6);
  REQUIRE(r.trades.size()==3); REQUIRE(r.trades[0].resting_order_id==1); REQUIRE(r.trades[1].resting_order_id==2); REQUIRE(r.trades[2].resting_order_id==3);
  REQUIRE(r.trades[0].quantity==2); REQUIRE(r.trades[1].quantity==3); REQUIRE(r.trades[2].quantity==1);
  REQUIRE(e.book().best_ask()==102); REQUIRE(e.book().displayed_quantity_at(lob::core::Side::Sell,102)==3); consistent(e);
}
TEST_CASE("cancellation works before and after partial execution") {
  lob::exchange::MatchingEngine e; static_cast<void>(e.submit_limit(1,1,lob::core::Side::Sell,101,5));
  REQUIRE(e.cancel(1,2).accepted); REQUIRE_FALSE(e.book().find_order(1));
  static_cast<void>(e.submit_limit(2,3,lob::core::Side::Sell,101,5)); static_cast<void>(e.submit_market(3,4,lob::core::Side::Buy,2));
  const auto c=e.cancel(2,5); REQUIRE(c.accepted); REQUIRE(c.cancelled_order->remaining_quantity==3); consistent(e);
}
TEST_CASE("market residuals expire and invalid or repeated IDs are rejected") {
  lob::exchange::MatchingEngine e; const auto empty=e.submit_market(1,1,lob::core::Side::Buy,4);
  REQUIRE(empty.order.status==lob::exchange::OrderStatus::Expired); REQUIRE(empty.expired_market_quantity==4); REQUIRE(e.book().live_order_count()==0);
  REQUIRE_FALSE(e.submit_limit(0,2,lob::core::Side::Buy,100,1).accepted); REQUIRE(e.submit_limit(2,3,lob::core::Side::Buy,100,1).accepted);
  const auto duplicate=e.submit_market(2,4,lob::core::Side::Sell,1); REQUIRE_FALSE(duplicate.accepted); REQUIRE(e.book().best_bid()==100); consistent(e);
}
TEST_CASE("events record execution and submissions") {
  lob::exchange::MatchingEngine e; static_cast<void>(e.submit_limit(1,1,lob::core::Side::Sell,100,2)); static_cast<void>(e.submit_market(2,2,lob::core::Side::Buy,2));
  REQUIRE(e.trades().size()==1); REQUIRE(e.events().size()==3); REQUIRE(e.events()[1].type==lob::exchange::ExchangeEventType::Execution); REQUIRE(e.events()[1].trade.has_value()); consistent(e);
}
