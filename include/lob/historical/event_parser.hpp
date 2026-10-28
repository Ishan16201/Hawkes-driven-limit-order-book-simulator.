#pragma once

#include "lob/historical/types.hpp"
#include "lob/simulation/simulator.hpp"

#include <istream>
#include <string>
#include <vector>

namespace lob::historical {

class EventParser final {
 public:
  /// Parses NDJSON formatted historical records from an input stream.
  [[nodiscard]] static std::pair<std::vector<RawEventRecord>, IngestionValidationReport>
  parse_ndjson(std::istream& stream, const std::string& source_identity = "ndjson_stream");

  /// Parses CSV formatted historical records from an input stream.
  /// Expects header: version,timestamp_ns,source_sequence,event_type,side,price_ticks,quantity_lots,order_id,trade_id
  [[nodiscard]] static std::pair<std::vector<RawEventRecord>, IngestionValidationReport>
  parse_csv(std::istream& stream, const std::string& source_identity = "csv_stream");

  /// Maps raw records into canonical Hawkes events and validates sequencing,
  /// avoiding duplicate counting of exchange actions.
  [[nodiscard]] static std::pair<std::vector<MappedHistoricalEvent>, IngestionValidationReport>
  map_and_validate(const std::vector<RawEventRecord>& raw_records,
                   const EventMappingConfig& config = {},
                   const std::string& source_identity = "mapped_events");

  /// Converts mapped historical events into simulation CanonicalEvents for replay.
  [[nodiscard]] static std::vector<simulation::CanonicalEvent>
  to_simulation_events(const std::vector<MappedHistoricalEvent>& events);

  /// Generates a synthetic CSV fixture stream for testing parser and validation.
  [[nodiscard]] static std::string generate_synthetic_csv_fixture(
      std::size_t count = 20, core::TimestampNs interval_ns = 100'000'000);

  /// Generates a synthetic NDJSON fixture stream for testing parser and validation.
  [[nodiscard]] static std::string generate_synthetic_ndjson_fixture(
      std::size_t count = 20, core::TimestampNs interval_ns = 100'000'000);
};

}  // namespace lob::historical
