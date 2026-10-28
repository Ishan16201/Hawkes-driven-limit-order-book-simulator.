#include "lob/historical/event_parser.hpp"
#include "lob/historical/types.hpp"
#include "lob/simulation/simulator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

using namespace lob;
using namespace lob::historical;

TEST_CASE("Historical ingestion: synthetic CSV and NDJSON fixtures parse correctly", "[historical]") {
  SECTION("CSV fixture parsing") {
    const std::string csv_data = EventParser::generate_synthetic_csv_fixture(10, 50'000'000);
    std::istringstream stream(csv_data);
    const auto [records, report] = EventParser::parse_csv(stream, "test_fixture.csv");

    REQUIRE(records.size() == 10);
    REQUIRE(report.total_records_read == 10);
    REQUIRE(report.malformed_records == 0);
    REQUIRE(records.front().timestamp_ns == 100'000'000);
    REQUIRE(records.back().timestamp_ns == 100'000'000 + 9 * 50'000'000);
  }

  SECTION("NDJSON fixture parsing") {
    const std::string ndjson_data = EventParser::generate_synthetic_ndjson_fixture(10, 50'000'000);
    std::istringstream stream(ndjson_data);
    const auto [records, report] = EventParser::parse_ndjson(stream, "test_fixture.ndjson");

    REQUIRE(records.size() == 10);
    REQUIRE(report.total_records_read == 10);
    REQUIRE(report.malformed_records == 0);
  }
}

TEST_CASE("Historical validation: detects malformed lines, out-of-order timestamps, and duplicate sequences", "[historical]") {
  SECTION("Malformed line detection") {
    const std::string bad_csv =
        "version,timestamp_ns,source_sequence,event_type,side,price_ticks,quantity_lots\n"
        "1,100,1,SUBMIT,BUY,100,10\n"
        "bad_line_with_insufficient_cols\n"
        "1,300,2,SUBMIT,SELL,102,10\n";
    std::istringstream stream(bad_csv);
    const auto [records, report] = EventParser::parse_csv(stream, "bad.csv");

    REQUIRE(records.size() == 2);
    REQUIRE(report.malformed_records == 1);
    REQUIRE(!report.validation_errors.empty());
  }

  SECTION("Timestamp inversion detection") {
    std::vector<RawEventRecord> raw;
    raw.push_back({.timestamp_ns = 200, .source_sequence = 1, .event_type = RawFeedEventType::OrderSubmit, .side = core::Side::Buy});
    raw.push_back({.timestamp_ns = 100, .source_sequence = 2, .event_type = RawFeedEventType::OrderSubmit, .side = core::Side::Buy});  // Inversion!

    const auto [mapped, report] = EventParser::map_and_validate(raw);
    REQUIRE(report.timestamp_order_violations == 1);
    REQUIRE(!report.is_valid());
  }

  SECTION("Duplicate sequence detection") {
    std::vector<RawEventRecord> raw;
    raw.push_back({.timestamp_ns = 100, .source_sequence = 42, .event_type = RawFeedEventType::OrderSubmit, .side = core::Side::Buy});
    raw.push_back({.timestamp_ns = 200, .source_sequence = 42, .event_type = RawFeedEventType::OrderSubmit, .side = core::Side::Buy});  // Duplicate!

    const auto [mapped, report] = EventParser::map_and_validate(raw);
    REQUIRE(report.duplicate_sequences == 1);
    REQUIRE(!report.validation_warnings.empty());
  }
}

TEST_CASE("Event mapping: canonical taxonomy, replace decomposition, and deduplication", "[historical]") {
  std::vector<RawEventRecord> raw;

  // 1. Submit Buy -> LB
  raw.push_back({.timestamp_ns = 100, .source_sequence = 1, .event_type = RawFeedEventType::OrderSubmit, .side = core::Side::Buy, .price_ticks = 99, .quantity_lots = 10, .order_id = 1});

  // 2. Submit Sell -> LA
  raw.push_back({.timestamp_ns = 200, .source_sequence = 2, .event_type = RawFeedEventType::OrderSubmit, .side = core::Side::Sell, .price_ticks = 101, .quantity_lots = 10, .order_id = 2});

  // 3. Execution (Aggressive Buy) -> MB
  raw.push_back({.timestamp_ns = 300, .source_sequence = 3, .event_type = RawFeedEventType::OrderExecution, .side = core::Side::Buy, .price_ticks = 101, .quantity_lots = 5, .order_id = 3, .trade_id = 1001});

  // 4. Duplicate Trade Execution with same trade_id -> should be suppressed from double-counting!
  raw.push_back({.timestamp_ns = 300, .source_sequence = 4, .event_type = RawFeedEventType::OrderExecution, .side = core::Side::Buy, .price_ticks = 101, .quantity_lots = 5, .order_id = 3, .trade_id = 1001});

  // 5. Replace -> decomposed into CancelBuy + LimitBuy
  raw.push_back({.timestamp_ns = 400, .source_sequence = 5, .event_type = RawFeedEventType::OrderReplace, .side = core::Side::Buy, .price_ticks = 98, .quantity_lots = 10, .order_id = 1});

  // 6. Feed status -> skipped without error
  raw.push_back({.timestamp_ns = 500, .source_sequence = 6, .event_type = RawFeedEventType::FeedStatus});

  EventMappingConfig config;
  config.replace_policy = ReplaceHandlingPolicy::DecomposeToCancelAndSubmit;
  config.deduplicate_executions = true;

  const auto [mapped, report] = EventParser::map_and_validate(raw, config);

  // Mapped events:
  // 1: LB
  // 2: LA
  // 3: MB
  // (4 skipped as duplicate execution)
  // 5a: CB (from replace)
  // 5b: LB (from replace)
  // (6 skipped as feed status)
  // Total = 5 mapped events
  REQUIRE(mapped.size() == 5);
  REQUIRE(mapped[0].hawkes_type == stochastic::EventType::LimitBuy);
  REQUIRE(mapped[1].hawkes_type == stochastic::EventType::LimitSell);
  REQUIRE(mapped[2].hawkes_type == stochastic::EventType::MarketBuy);
  REQUIRE(mapped[3].hawkes_type == stochastic::EventType::CancelBuy);
  REQUIRE(mapped[4].hawkes_type == stochastic::EventType::LimitBuy);

  REQUIRE(report.feed_status_messages_skipped == 1);
  REQUIRE(report.has_order_level_data == true);
  REQUIRE(report.has_trade_level_data == true);
}

TEST_CASE("Historical events convert to simulator CanonicalEvents for deterministic replay", "[historical]") {
  std::vector<MappedHistoricalEvent> mapped;
  mapped.push_back({.timestamp_ns = 1'000'000, .sequence = 1, .hawkes_type = stochastic::EventType::LimitBuy, .price_ticks = 100, .quantity_lots = 5, .order_id = 10});
  mapped.push_back({.timestamp_ns = 2'000'000, .sequence = 2, .hawkes_type = stochastic::EventType::LimitSell, .price_ticks = 102, .quantity_lots = 5, .order_id = 11});

  const auto sim_events = EventParser::to_simulation_events(mapped);
  REQUIRE(sim_events.size() == 2);
  REQUIRE(sim_events[0].type == simulation::CommandType::Limit);
  REQUIRE(sim_events[0].side == core::Side::Buy);
  REQUIRE(sim_events[0].price_ticks == 100);
  REQUIRE(sim_events[1].side == core::Side::Sell);
  REQUIRE(sim_events[1].price_ticks == 102);

  // Feed to simulator replay source
  simulation::HistoricalReplaySource replay_source(sim_events, "historical_replay_test");
  REQUIRE(replay_source.events().size() == 2);
  REQUIRE(replay_source.identity() == "historical_replay_test");
}

TEST_CASE("deterministic CSV fixture replays partial fill and cancellation",
          "[historical][fixture]") {
  std::ifstream input(std::string{LOB_FIXTURE_DIR} +
                      "/deterministic_replay.csv");
  REQUIRE(input.good());
  const auto [raw, parse_report] =
      EventParser::parse_csv(input, "deterministic_replay.csv");
  REQUIRE(parse_report.malformed_records == 0U);
  REQUIRE(raw.size() == 4U);

  const auto [mapped, mapping_report] =
      EventParser::map_and_validate(raw);
  REQUIRE(mapping_report.is_valid());
  const auto events = EventParser::to_simulation_events(mapped);
  REQUIRE(events.size() == 4U);
  REQUIRE(events[2].type == simulation::CommandType::Market);
  REQUIRE(events[2].price_ticks == 0);
  REQUIRE(events[3].type == simulation::CommandType::Cancel);
  REQUIRE(events[3].price_ticks == 0);
  REQUIRE(events[3].quantity == 0U);

  simulation::HistoricalReplaySource source(events, "deterministic-fixture");
  simulation::Simulator simulator;
  const auto result = simulator.run({}, source);
  REQUIRE(result.trades.size() == 1U);
  REQUIRE(result.trades[0].resting_order_id == 1U);
  REQUIRE(result.trades[0].incoming_order_id == 3U);
  REQUIRE(result.trades[0].price_ticks == 101);
  REQUIRE(result.trades[0].quantity == 3U);
  REQUIRE(result.summary.final_best_ask == 101);
  REQUIRE(result.summary.final_live_orders == 1U);
}
