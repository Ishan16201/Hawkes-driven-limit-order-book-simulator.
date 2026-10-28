#include "lob/simulation/simulator.hpp"
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <stdexcept>

namespace {
lob::simulation::CanonicalEvent limit(const lob::core::EventId event_id, const lob::core::TimestampNs time, const std::uint64_t sequence, const lob::core::OrderId order_id, const lob::core::Side side, const lob::core::PriceTicks price, const lob::core::QuantityLots quantity) { return {.event_id=event_id,.timestamp_ns=time,.sequence=sequence,.type=lob::simulation::CommandType::Limit,.order_id=order_id,.side=side,.price_ticks=price,.quantity=quantity}; }
}
TEST_CASE("same timestamp commands use ascending sequence independent of source order") {
  lob::simulation::DeterministicEventSource source({limit(2,10,1,2,lob::core::Side::Buy,100,1),limit(1,10,0,1,lob::core::Side::Sell,100,1)},"hand");
  lob::simulation::Simulator simulator; const auto r=simulator.run({},source);
  REQUIRE(r.dispatches[0].event.event_id==1); REQUIRE(r.dispatches[1].event.event_id==2); REQUIRE(r.trades.size()==1); REQUIRE(r.summary.final_live_orders==0); REQUIRE(r.trades[0].resting_order_id==1);
}
TEST_CASE("seeded streams and replay are reproducible") {
  const lob::simulation::SyntheticStreamConfig c{.seed=42,.first_timestamp_ns=0,.interval_ns=5,.event_count=5,.first_order_id=1,.center_price_ticks=100,.quantity_lots=1};
  const auto a=lob::simulation::generate_seeded_limit_events(c); const auto b=lob::simulation::generate_seeded_limit_events(c); REQUIRE(a.size()==b.size()); REQUIRE(a[3].price_ticks==b[3].price_ticks); REQUIRE(a[3].side==b[3].side);
  lob::simulation::HistoricalReplaySource replay(a,"normalized-fixture"); lob::simulation::Simulator simulator; const auto first=simulator.run({.random_seed=42},replay); const auto second=simulator.run({.random_seed=42},replay);
  REQUIRE(first.trades==second.trades); REQUIRE(first.summary.traded_quantity==second.summary.traded_quantity); REQUIRE(first.summary.final_best_bid==second.summary.final_best_bid); REQUIRE(lob::simulation::Simulator::event_log_ndjson(first)==lob::simulation::Simulator::event_log_ndjson(second));
}
TEST_CASE("configuration and canonical event validation reject invalid input") {
  lob::simulation::Simulator simulator;
  lob::simulation::DeterministicEventSource duplicate_sequence({limit(1,1,0,1,lob::core::Side::Buy,100,1),limit(2,2,0,2,lob::core::Side::Buy,99,1)},"bad");
  REQUIRE_THROWS_AS(simulator.run({},duplicate_sequence),std::invalid_argument);
  lob::simulation::DeterministicEventSource invalid_market({{.event_id=1,.timestamp_ns=0,.sequence=0,.type=lob::simulation::CommandType::Market,.order_id=1,.side=lob::core::Side::Buy,.price_ticks=1,.quantity=1}},"bad-market");
  REQUIRE_THROWS_AS(simulator.run({},invalid_market),std::invalid_argument);
  REQUIRE_THROWS_AS(simulator.run({.event_schema_version=99},duplicate_sequence),std::invalid_argument);
}

TEST_CASE("seeded event generation rejects integer timestamp and ID overflow") {
  lob::simulation::SyntheticStreamConfig config{
      .seed = 1U,
      .first_timestamp_ns = 0,
      .interval_ns = 1,
      .event_count = 2U,
      .first_order_id = std::numeric_limits<lob::core::OrderId>::max(),
      .center_price_ticks = 100,
      .quantity_lots = 1U};
  REQUIRE_THROWS_AS(lob::simulation::generate_seeded_limit_events(config),
                    std::overflow_error);

  config.first_order_id = 1U;
  config.first_timestamp_ns =
      std::numeric_limits<lob::core::TimestampNs>::max() - 1;
  config.interval_ns = 2;
  REQUIRE_THROWS_AS(lob::simulation::generate_seeded_limit_events(config),
                    std::overflow_error);
}
TEST_CASE("logging can be disabled without changing simulated state") {
  lob::simulation::DeterministicEventSource source({limit(1,1,0,1,lob::core::Side::Buy,100,2)},"one"); lob::simulation::Simulator simulator;
  const auto logged=simulator.run({.enable_event_log=true},source); const auto quiet=simulator.run({.enable_event_log=false},source);
  REQUIRE(logged.dispatches.size()==1); REQUIRE(quiet.dispatches.empty()); REQUIRE(logged.summary.final_best_bid==quiet.summary.final_best_bid); REQUIRE(logged.summary.final_live_orders==quiet.summary.final_live_orders);
  const auto serialized = lob::simulation::Simulator::event_log_ndjson(logged);
  REQUIRE(serialized.find("\"command_type_name\":\"limit\"") != std::string::npos);
  REQUIRE(serialized.find("\"price_ticks\":100") != std::string::npos);
  REQUIRE(serialized.find("\"quantity_lots\":2") != std::string::npos);
}
