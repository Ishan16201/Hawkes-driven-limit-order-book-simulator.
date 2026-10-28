#include "lob/simulation/simulator.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace lob::simulation {
DeterministicEventSource::DeterministicEventSource(std::vector<CanonicalEvent> events, std::string identity) : events_(std::move(events)), identity_(std::move(identity)) {}
std::vector<CanonicalEvent> DeterministicEventSource::events() const { return events_; }
std::string DeterministicEventSource::identity() const { return identity_; }
Mt19937_64Rng::Mt19937_64Rng(const std::uint64_t seed) : seed_(seed), engine_(seed) {}
std::uint64_t Mt19937_64Rng::next_u64() { return engine_(); }
std::uint64_t Mt19937_64Rng::seed() const noexcept { return seed_; }

std::vector<CanonicalEvent> generate_seeded_limit_events(const SyntheticStreamConfig& c) {
  if (!core::is_valid_timestamp(c.first_timestamp_ns) || c.interval_ns <= 0 ||
      c.event_count == 0U || c.first_order_id == 0U ||
      c.center_price_ticks <= 1 ||
      !core::is_valid_quantity(c.quantity_lots)) {
    throw std::invalid_argument{"invalid synthetic stream configuration"};
  }
  const auto last_index = static_cast<std::uint64_t>(c.event_count - 1U);
  if (c.event_count >
          static_cast<std::size_t>(std::numeric_limits<core::EventId>::max()) ||
      last_index >
          std::numeric_limits<core::OrderId>::max() - c.first_order_id) {
    throw std::overflow_error{"synthetic event or order ID overflow"};
  }
  const auto remaining_timestamp_range =
      std::numeric_limits<core::TimestampNs>::max() - c.first_timestamp_ns;
  if (last_index >
      static_cast<std::uint64_t>(remaining_timestamp_range / c.interval_ns)) {
    throw std::overflow_error{"synthetic timestamp overflow"};
  }

  Mt19937_64Rng rng{c.seed};
  std::vector<CanonicalEvent> result;
  result.reserve(c.event_count);
  for (std::size_t i = 0; i < c.event_count; ++i) {
    const bool buy = (rng.next_u64() & 1U) == 0U;
    const auto offset =
        static_cast<core::PriceTicks>((rng.next_u64() % 3U) + 1U);
    core::PriceTicks price{};
    if (buy) {
      price = offset >= c.center_price_ticks
                  ? 1
                  : c.center_price_ticks - offset;
    } else {
      if (c.center_price_ticks >
          std::numeric_limits<core::PriceTicks>::max() - offset) {
        throw std::overflow_error{"synthetic ask price overflow"};
      }
      price = c.center_price_ticks + offset;
    }
    const auto index = static_cast<std::uint64_t>(i);
    result.push_back(CanonicalEvent{
        .event_id = static_cast<core::EventId>(index + 1U),
        .timestamp_ns =
            c.first_timestamp_ns +
            static_cast<core::TimestampNs>(index) * c.interval_ns,
        .sequence = index,
        .type = CommandType::Limit,
        .order_id = c.first_order_id + index,
        .side = buy ? core::Side::Buy : core::Side::Sell,
        .price_ticks = price,
        .quantity = c.quantity_lots});
  }
  return result;
}

void SimulationClock::advance_to(const core::TimestampNs timestamp) { core::require_valid_timestamp(timestamp); if (now_ && timestamp < *now_) throw std::invalid_argument{"simulation clock cannot move backward"}; now_=timestamp; }
std::optional<core::TimestampNs> SimulationClock::now() const noexcept { return now_; }

void Simulator::validate_config(const SimulationConfig& c) { if (c.event_schema_version != kEventSchemaVersion || c.configuration_id.empty() || c.software_revision.empty()) throw std::invalid_argument{"invalid simulation configuration"}; }
void Simulator::validate_events(const std::vector<CanonicalEvent>& events) {
  std::unordered_set<core::EventId> ids; std::unordered_set<std::uint64_t> sequences;
  for (const auto& e: events) {
    if (e.event_id==0U || !ids.insert(e.event_id).second || !sequences.insert(e.sequence).second || !core::is_valid_timestamp(e.timestamp_ns) || e.order_id==0U) throw std::invalid_argument{"invalid or nonunique canonical event"};
    if ((e.type==CommandType::Limit && (!core::is_valid_side(e.side) || e.price_ticks<=0 || !core::is_valid_quantity(e.quantity))) || (e.type==CommandType::Market && (!core::is_valid_side(e.side) || e.price_ticks!=0 || !core::is_valid_quantity(e.quantity))) || (e.type==CommandType::Cancel && (e.price_ticks!=0 || e.quantity!=0U))) throw std::invalid_argument{"invalid canonical command fields"};
  }
}

SimulationResult Simulator::run(const SimulationConfig& config, const EventSource& source) {
  validate_config(config); auto events=source.events(); validate_events(events);
  std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.timestamp_ns != b.timestamp_ns ? a.timestamp_ns < b.timestamp_ns : a.sequence < b.sequence; });
  exchange::MatchingEngine engine; SimulationClock clock; SimulationResult result{.metadata={.event_schema_version=config.event_schema_version,.software_revision=config.software_revision,.configuration_id=config.configuration_id,.input_identity=source.identity(),.random_seed=config.random_seed}};
  for (const auto& e:events) {
    clock.advance_to(e.timestamp_ns); const auto before=engine.trades().size(); bool accepted=false; auto rejection=exchange::RejectionReason::None;
    if (e.type==CommandType::Limit) { const auto r=engine.submit_limit(e.order_id,e.timestamp_ns,e.side,e.price_ticks,e.quantity); accepted=r.accepted; rejection=r.rejection; }
    else if (e.type==CommandType::Market) { const auto r=engine.submit_market(e.order_id,e.timestamp_ns,e.side,e.quantity); accepted=r.accepted; rejection=r.rejection; }
    else { const auto r=engine.cancel(e.order_id,e.timestamp_ns); accepted=r.accepted; rejection=r.rejection; }
    if (config.enable_event_log) result.dispatches.push_back(DispatchRecord{.event=e,.accepted=accepted,.rejection=rejection,.trades_created=engine.trades().size()-before});
    ++result.summary.events_dispatched; if (accepted) ++result.summary.accepted_actions; else ++result.summary.rejected_actions;
  }
  result.trades=engine.trades(); result.summary.trades=result.trades.size(); for (const auto& trade:result.trades) {
    if (std::numeric_limits<core::QuantityLots>::max() -
            result.summary.traded_quantity <
        trade.quantity) {
      throw std::overflow_error{"simulation traded-quantity summary overflow"};
    }
    result.summary.traded_quantity+=trade.quantity;
  }
  result.summary.final_best_bid=engine.book().best_bid(); result.summary.final_best_ask=engine.book().best_ask(); result.summary.final_live_orders=engine.book().live_order_count(); return result;
}

std::string Simulator::event_log_ndjson(const SimulationResult& r) {
  std::ostringstream out;
  for (const auto& dispatch : r.dispatches) {
    const char* command_type = "cancel";
    if (dispatch.event.type == CommandType::Limit) {
      command_type = "limit";
    } else if (dispatch.event.type == CommandType::Market) {
      command_type = "market";
    }
    out << "{\"schema_version\":" << r.metadata.event_schema_version
        << ",\"event_id\":" << dispatch.event.event_id
        << ",\"timestamp_ns\":" << dispatch.event.timestamp_ns
        << ",\"sequence\":" << dispatch.event.sequence
        << ",\"command_type\":"
        << static_cast<unsigned>(dispatch.event.type)
        << ",\"command_type_name\":\"" << command_type
        << "\",\"order_id\":" << dispatch.event.order_id
        << ",\"side\":\""
        << (dispatch.event.side == core::Side::Buy ? "BUY" : "SELL")
        << "\",\"price_ticks\":" << dispatch.event.price_ticks
        << ",\"quantity_lots\":" << dispatch.event.quantity
        << ",\"accepted\":" << (dispatch.accepted ? "true" : "false")
        << ",\"rejection\":" << static_cast<unsigned>(dispatch.rejection)
        << ",\"trades_created\":" << dispatch.trades_created << "}\n";
  }
  return out.str();
}
}  // namespace lob::simulation
