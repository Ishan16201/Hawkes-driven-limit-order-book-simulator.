#include "lob/historical/event_parser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace lob::historical {

std::string raw_event_type_to_string(const RawFeedEventType type) {
  switch (type) {
    case RawFeedEventType::OrderSubmit: return "SUBMIT";
    case RawFeedEventType::OrderExecution: return "EXECUTE";
    case RawFeedEventType::OrderCancel: return "CANCEL";
    case RawFeedEventType::OrderReplace: return "REPLACE";
    case RawFeedEventType::FeedStatus: return "STATUS";
    case RawFeedEventType::Unknown: return "UNKNOWN";
  }
  return "UNKNOWN";
}

RawFeedEventType raw_event_type_from_string(const std::string& str) {
  std::string upper;
  upper.reserve(str.size());
  for (const char c : str) {
    upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  if (upper == "SUBMIT" || upper == "ADD" || upper == "NEW" || upper == "LIMIT") {
    return RawFeedEventType::OrderSubmit;
  }
  if (upper == "EXECUTE" || upper == "TRADE" || upper == "FILL") {
    return RawFeedEventType::OrderExecution;
  }
  if (upper == "CANCEL" || upper == "DELETE") {
    return RawFeedEventType::OrderCancel;
  }
  if (upper == "REPLACE" || upper == "MODIFY" || upper == "AMEND") {
    return RawFeedEventType::OrderReplace;
  }
  if (upper == "STATUS" || upper == "HEARTBEAT" || upper == "HALT" || upper == "RESET") {
    return RawFeedEventType::FeedStatus;
  }
  return RawFeedEventType::Unknown;
}

std::string IngestionValidationReport::summary_text() const {
  std::ostringstream out;
  out << "Ingestion Validation Report [" << source_identity << "]\n"
      << "  Status:                      " << (is_valid() ? "PASSED" : "FAILED") << "\n"
      << "  Total Records Read:          " << total_records_read << "\n"
      << "  Valid Canonical Events:      " << valid_canonical_events << "\n"
      << "  Malformed Records:           " << malformed_records << "\n"
      << "  Timestamp Order Violations:  " << timestamp_order_violations << "\n"
      << "  Duplicate Sequence Numbers:  " << duplicate_sequences << "\n"
      << "  Feed Status Messages Skipped:" << feed_status_messages_skipped << "\n"
      << "  Has Order Identifiers:       " << (has_order_level_data ? "YES" : "NO") << "\n"
      << "  Has Trade Identifiers:       " << (has_trade_level_data ? "YES" : "NO") << "\n"
      << "  Timestamp Range (ns):        [" << min_timestamp_ns << ", " << max_timestamp_ns << "]\n";

  if (!validation_errors.empty()) {
    out << "  Errors (" << validation_errors.size() << "):\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(5, validation_errors.size()); ++i) {
      out << "    * " << validation_errors[i] << "\n";
    }
    if (validation_errors.size() > 5) {
      out << "    ... (" << validation_errors.size() - 5 << " more)\n";
    }
  }

  if (!validation_warnings.empty()) {
    out << "  Warnings (" << validation_warnings.size() << "):\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(5, validation_warnings.size()); ++i) {
      out << "    * " << validation_warnings[i] << "\n";
    }
  }

  return out.str();
}

namespace {

void trim(std::string& s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
}

std::vector<std::string> split_csv_line(const std::string& line) {
  std::vector<std::string> tokens;
  std::stringstream ss(line);
  std::string item;
  while (std::getline(ss, item, ',')) {
    trim(item);
    tokens.push_back(item);
  }
  return tokens;
}

}  // namespace

std::pair<std::vector<RawEventRecord>, IngestionValidationReport>
EventParser::parse_csv(std::istream& stream, const std::string& source_identity) {
  std::vector<RawEventRecord> records;
  IngestionValidationReport report;
  report.source_identity = source_identity;

  std::string line;
  std::size_t line_num = 0;
  bool header_seen = false;

  while (std::getline(stream, line)) {
    ++line_num;
    trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const auto tokens = split_csv_line(line);
    if (!header_seen) {
      // Check if this is the header row
      if (!tokens.empty() && (tokens[0] == "version" || tokens[0] == "timestamp_ns" || tokens[0] == "schema_version")) {
        header_seen = true;
        continue;
      }
      header_seen = true;
    }

    ++report.total_records_read;
    if (tokens.size() < 7) {
      ++report.malformed_records;
      report.validation_errors.push_back("Line " + std::to_string(line_num) +
                                         ": insufficient columns (expected >= 7, got " +
                                         std::to_string(tokens.size()) + ")");
      continue;
    }

    try {
      RawEventRecord record;
      // version,timestamp_ns,source_sequence,event_type,side,price_ticks,quantity_lots,order_id,trade_id
      record.schema_version = static_cast<std::uint32_t>(std::stoul(tokens[0]));
      record.timestamp_ns = static_cast<core::TimestampNs>(std::stoll(tokens[1]));
      record.source_sequence = static_cast<std::uint64_t>(std::stoull(tokens[2]));
      record.event_type = raw_event_type_from_string(tokens[3]);

      std::string side_str = tokens[4];
      trim(side_str);
      for (char& c : side_str) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      record.side = (side_str == "BUY" || side_str == "B" || side_str == "1") ? core::Side::Buy
                                                                               : core::Side::Sell;

      record.price_ticks = static_cast<core::PriceTicks>(std::stoll(tokens[5]));
      record.quantity_lots = static_cast<core::QuantityLots>(std::stoull(tokens[6]));

      if (tokens.size() > 7 && !tokens[7].empty()) {
        record.order_id = static_cast<core::OrderId>(std::stoull(tokens[7]));
      }
      if (tokens.size() > 8 && !tokens[8].empty()) {
        record.trade_id = static_cast<core::TradeId>(std::stoull(tokens[8]));
      }

      records.push_back(record);
    } catch (const std::exception& e) {
      ++report.malformed_records;
      report.validation_errors.push_back("Line " + std::to_string(line_num) +
                                         ": parsing error: " + e.what());
    }
  }

  return {records, report};
}

std::pair<std::vector<RawEventRecord>, IngestionValidationReport>
EventParser::parse_ndjson(std::istream& stream, const std::string& source_identity) {
  std::vector<RawEventRecord> records;
  IngestionValidationReport report;
  report.source_identity = source_identity;

  std::string line;
  std::size_t line_num = 0;

  auto extract_field = [](const std::string& json, const std::string& key) -> std::optional<std::string> {
    const std::string pattern = "\"" + key + "\"";
    const auto pos = json.find(pattern);
    if (pos == std::string::npos) {
      return std::nullopt;
    }
    const auto colon = json.find(':', pos + pattern.size());
    if (colon == std::string::npos) {
      return std::nullopt;
    }
    auto start = colon + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) {
      ++start;
    }
    if (start >= json.size()) {
      return std::nullopt;
    }

    if (json[start] == '"') {
      // String value
      const auto end_q = json.find('"', start + 1);
      if (end_q == std::string::npos) return std::nullopt;
      return json.substr(start + 1, end_q - (start + 1));
    }
    // Number or bool value
    auto end_val = json.find_first_of(",}\n\r", start);
    if (end_val == std::string::npos) {
      end_val = json.size();
    }
    std::string val = json.substr(start, end_val - start);
    trim(val);
    return val;
  };

  while (std::getline(stream, line)) {
    ++line_num;
    trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }

    ++report.total_records_read;
    try {
      RawEventRecord record;
      const auto ts_str = extract_field(line, "timestamp_ns");
      const auto type_str = extract_field(line, "event_type");
      const auto side_str = extract_field(line, "side");
      const auto price_str = extract_field(line, "price_ticks");
      const auto qty_str = extract_field(line, "quantity_lots");

      if (!ts_str || !type_str || !side_str) {
        ++report.malformed_records;
        report.validation_errors.push_back("Line " + std::to_string(line_num) +
                                           ": missing required JSON fields");
        continue;
      }

      record.timestamp_ns = static_cast<core::TimestampNs>(std::stoll(*ts_str));
      record.event_type = raw_event_type_from_string(*type_str);

      std::string side_u = *side_str;
      for (char& c : side_u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      record.side = (side_u == "BUY" || side_u == "B") ? core::Side::Buy : core::Side::Sell;

      record.price_ticks = price_str ? static_cast<core::PriceTicks>(std::stoll(*price_str)) : 0;
      record.quantity_lots = qty_str ? static_cast<core::QuantityLots>(std::stoull(*qty_str)) : 0;

      const auto seq_str = extract_field(line, "sequence");
      if (seq_str) {
        record.source_sequence = static_cast<std::uint64_t>(std::stoull(*seq_str));
      }

      const auto ord_str = extract_field(line, "order_id");
      if (ord_str) {
        record.order_id = static_cast<core::OrderId>(std::stoull(*ord_str));
      }
      const auto tr_str = extract_field(line, "trade_id");
      if (tr_str) {
        record.trade_id = static_cast<core::TradeId>(std::stoull(*tr_str));
      }

      records.push_back(record);
    } catch (const std::exception& e) {
      ++report.malformed_records;
      report.validation_errors.push_back("Line " + std::to_string(line_num) +
                                         ": JSON error: " + e.what());
    }
  }

  return {records, report};
}

std::pair<std::vector<MappedHistoricalEvent>, IngestionValidationReport>
EventParser::map_and_validate(const std::vector<RawEventRecord>& raw_records,
                             const EventMappingConfig& config,
                             const std::string& source_identity) {
  std::vector<MappedHistoricalEvent> mapped;
  IngestionValidationReport report;
  report.source_identity = source_identity;
  report.total_records_read = raw_records.size();

  if (raw_records.empty()) {
    report.validation_warnings.push_back("Raw record collection is empty");
    return {mapped, report};
  }

  report.min_timestamp_ns = raw_records.front().timestamp_ns;
  report.max_timestamp_ns = raw_records.front().timestamp_ns;

  std::unordered_set<std::uint64_t> seen_sequences;
  std::unordered_set<core::TradeId> seen_trade_ids;
  core::TimestampNs last_ts = -1;

  for (std::size_t i = 0; i < raw_records.size(); ++i) {
    const auto& rec = raw_records[i];

    // Timestamp tracking and ordering check
    report.min_timestamp_ns = std::min(report.min_timestamp_ns, rec.timestamp_ns);
    report.max_timestamp_ns = std::max(report.max_timestamp_ns, rec.timestamp_ns);

    if (config.require_positive_timestamps && rec.timestamp_ns <= 0) {
      report.validation_errors.push_back("Record " + std::to_string(i) +
                                         ": non-positive timestamp " +
                                         std::to_string(rec.timestamp_ns));
      ++report.malformed_records;
    }

    if (last_ts >= 0 && rec.timestamp_ns < last_ts) {
      ++report.timestamp_order_violations;
      report.validation_errors.push_back(
          "Record " + std::to_string(i) + ": timestamp inversion (" +
          std::to_string(rec.timestamp_ns) + " < " + std::to_string(last_ts) + ")");
    }
    last_ts = rec.timestamp_ns;

    // Sequence tracking
    if (rec.source_sequence > 0) {
      if (!seen_sequences.insert(rec.source_sequence).second) {
        ++report.duplicate_sequences;
        report.validation_warnings.push_back(
            "Record " + std::to_string(i) + ": duplicate sequence " +
            std::to_string(rec.source_sequence));
      }
    }

    // Skip feed status/control messages
    if (rec.event_type == RawFeedEventType::FeedStatus) {
      ++report.feed_status_messages_skipped;
      continue;
    }

    // Identifiers check: verify if order-level IDs and trade IDs are present
    const bool is_order_action = (rec.event_type == RawFeedEventType::OrderSubmit ||
                                  rec.event_type == RawFeedEventType::OrderCancel ||
                                  rec.event_type == RawFeedEventType::OrderReplace);
    if (is_order_action && !rec.order_id.has_value()) {
      report.has_order_level_data = false;
    }
    if (rec.event_type == RawFeedEventType::OrderExecution && !rec.trade_id.has_value()) {
      report.has_trade_level_data = false;
    }

    // Execution deduplication check
    if (rec.event_type == RawFeedEventType::OrderExecution && config.deduplicate_executions) {
      if (rec.trade_id.has_value()) {
        if (!seen_trade_ids.insert(*rec.trade_id).second) {
          // Already seen trade execution for this match ID -> skip to avoid double-counting
          report.validation_warnings.push_back(
              "Record " + std::to_string(i) + ": duplicate execution trade ID " +
              std::to_string(*rec.trade_id) + " skipped");
          continue;
        }
      }
    }

    // Mapping raw feed types to canonical 6D Hawkes event types
    const bool is_buy = (rec.side == core::Side::Buy);

    if (rec.event_type == RawFeedEventType::OrderSubmit) {
      MappedHistoricalEvent ev;
      ev.timestamp_ns = rec.timestamp_ns;
      ev.sequence = rec.source_sequence;
      ev.hawkes_type = is_buy ? stochastic::EventType::LimitBuy
                              : stochastic::EventType::LimitSell;
      ev.price_ticks = rec.price_ticks;
      ev.quantity_lots = rec.quantity_lots;
      ev.order_id = rec.order_id;
      mapped.push_back(ev);
    } else if (rec.event_type == RawFeedEventType::OrderCancel) {
      MappedHistoricalEvent ev;
      ev.timestamp_ns = rec.timestamp_ns;
      ev.sequence = rec.source_sequence;
      ev.hawkes_type = is_buy ? stochastic::EventType::CancelBuy
                              : stochastic::EventType::CancelSell;
      ev.price_ticks = rec.price_ticks;
      ev.quantity_lots = rec.quantity_lots;
      ev.order_id = rec.order_id;
      mapped.push_back(ev);
    } else if (rec.event_type == RawFeedEventType::OrderExecution) {
      // Aggressive flow: buyer vs seller initiated
      MappedHistoricalEvent ev;
      ev.timestamp_ns = rec.timestamp_ns;
      ev.sequence = rec.source_sequence;
      ev.hawkes_type = is_buy ? stochastic::EventType::MarketBuy
                              : stochastic::EventType::MarketSell;
      ev.price_ticks = rec.price_ticks;
      ev.quantity_lots = rec.quantity_lots;
      ev.order_id = rec.order_id;
      ev.trade_id = rec.trade_id;
      mapped.push_back(ev);
    } else if (rec.event_type == RawFeedEventType::OrderReplace) {
      if (config.replace_policy == ReplaceHandlingPolicy::DecomposeToCancelAndSubmit) {
        // First cancel old
        MappedHistoricalEvent ev_canc;
        ev_canc.timestamp_ns = rec.timestamp_ns;
        ev_canc.sequence = rec.source_sequence;
        ev_canc.hawkes_type = is_buy ? stochastic::EventType::CancelBuy
                                     : stochastic::EventType::CancelSell;
        ev_canc.order_id = rec.order_id;
        mapped.push_back(ev_canc);

        // Then submit new
        MappedHistoricalEvent ev_sub;
        ev_sub.timestamp_ns = rec.timestamp_ns;
        ev_sub.sequence = rec.source_sequence;
        ev_sub.hawkes_type = is_buy ? stochastic::EventType::LimitBuy
                                    : stochastic::EventType::LimitSell;
        ev_sub.price_ticks = rec.price_ticks;
        ev_sub.quantity_lots = rec.quantity_lots;
        ev_sub.order_id = rec.order_id;
        mapped.push_back(ev_sub);
      }
    } else {
      report.validation_warnings.push_back(
          "Record " + std::to_string(i) + ": unmapped event type " +
          std::to_string(static_cast<int>(rec.event_type)));
    }
  }

  report.valid_canonical_events = mapped.size();
  return {mapped, report};
}

std::vector<simulation::CanonicalEvent>
EventParser::to_simulation_events(const std::vector<MappedHistoricalEvent>& events) {
  std::vector<simulation::CanonicalEvent> sim_events;
  sim_events.reserve(events.size());

  core::EventId next_eid = 1;
  for (const auto& ev : events) {
    simulation::CanonicalEvent sim;
    sim.event_id = next_eid++;
    sim.timestamp_ns = ev.timestamp_ns;
    sim.sequence = ev.sequence;
    sim.order_id = ev.order_id.value_or(0);

    if (stochastic::is_cancellation(ev.hawkes_type)) {
      sim.type = simulation::CommandType::Cancel;
      sim.side = stochastic::is_buy_side(ev.hawkes_type) ? core::Side::Buy : core::Side::Sell;
    } else if (stochastic::is_market_order(ev.hawkes_type)) {
      sim.type = simulation::CommandType::Market;
      sim.side = stochastic::is_buy_side(ev.hawkes_type) ? core::Side::Buy : core::Side::Sell;
      sim.quantity = ev.quantity_lots;
    } else {
      sim.type = simulation::CommandType::Limit;
      sim.side = stochastic::is_buy_side(ev.hawkes_type) ? core::Side::Buy : core::Side::Sell;
      sim.price_ticks = ev.price_ticks;
      sim.quantity = ev.quantity_lots;
    }
    sim_events.push_back(sim);
  }

  return sim_events;
}

std::string EventParser::generate_synthetic_csv_fixture(
    const std::size_t count, const core::TimestampNs interval_ns) {
  std::ostringstream out;
  out << "version,timestamp_ns,source_sequence,event_type,side,price_ticks,quantity_lots,order_id,trade_id\n";

  core::TimestampNs ts = 100'000'000;
  for (std::size_t i = 1; i <= count; ++i) {
    const std::string type_str = (i % 5 == 0) ? "CANCEL" : ((i % 7 == 0) ? "EXECUTE" : "SUBMIT");
    const std::string side_str = (i % 2 == 0) ? "BUY" : "SELL";
    const core::PriceTicks price = (i % 2 == 0) ? 99 : 101;
    out << "1," << ts << "," << i << "," << type_str << "," << side_str << ","
        << price << ",10," << i << "," << (type_str == "EXECUTE" ? std::to_string(i * 100) : "") << "\n";
    ts += interval_ns;
  }
  return out.str();
}

std::string EventParser::generate_synthetic_ndjson_fixture(
    const std::size_t count, const core::TimestampNs interval_ns) {
  std::ostringstream out;
  core::TimestampNs ts = 100'000'000;
  for (std::size_t i = 1; i <= count; ++i) {
    const std::string type_str = (i % 5 == 0) ? "CANCEL" : ((i % 7 == 0) ? "EXECUTE" : "SUBMIT");
    const std::string side_str = (i % 2 == 0) ? "BUY" : "SELL";
    const core::PriceTicks price = (i % 2 == 0) ? 99 : 101;
    out << "{\"version\":1,\"timestamp_ns\":" << ts
        << ",\"sequence\":" << i
        << ",\"event_type\":\"" << type_str << "\""
        << ",\"side\":\"" << side_str << "\""
        << ",\"price_ticks\":" << price
        << ",\"quantity_lots\":10"
        << ",\"order_id\":" << i;
    if (type_str == "EXECUTE") {
      out << ",\"trade_id\":" << (i * 100);
    }
    out << "}\n";
    ts += interval_ns;
  }
  return out.str();
}

}  // namespace lob::historical
