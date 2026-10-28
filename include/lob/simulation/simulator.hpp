#pragma once

#include "lob/exchange/matching_engine.hpp"

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace lob::simulation {

inline constexpr std::uint32_t kEventSchemaVersion = 1U;
enum class CommandType : std::uint8_t { Limit, Market, Cancel };

/// sequence is the required deterministic tie-breaker for equal timestamps.
struct CanonicalEvent final {
  core::EventId event_id{};
  core::TimestampNs timestamp_ns{};
  std::uint64_t sequence{};
  CommandType type{};
  core::OrderId order_id{};
  core::Side side{core::Side::Buy};
  core::PriceTicks price_ticks{};
  core::QuantityLots quantity{};
};

class EventSource {
 public:
  virtual ~EventSource() = default;
  [[nodiscard]] virtual std::vector<CanonicalEvent> events() const = 0;
  [[nodiscard]] virtual std::string identity() const = 0;
};

class DeterministicEventSource final : public EventSource {
 public:
  DeterministicEventSource(std::vector<CanonicalEvent> events, std::string source_identity);
  [[nodiscard]] std::vector<CanonicalEvent> events() const override;
  [[nodiscard]] std::string identity() const override;
 private:
  std::vector<CanonicalEvent> events_;
  std::string identity_;
};

/// Historical adapters will normalize source data into CanonicalEvent records
/// and construct this source; parsing vendor formats is intentionally deferred.
using HistoricalReplaySource = DeterministicEventSource;

class SeededRng {
 public:
  virtual ~SeededRng() = default;
  [[nodiscard]] virtual std::uint64_t next_u64() = 0;
  [[nodiscard]] virtual std::uint64_t seed() const noexcept = 0;
};

class Mt19937_64Rng final : public SeededRng {
 public:
  explicit Mt19937_64Rng(std::uint64_t seed);
  [[nodiscard]] std::uint64_t next_u64() override;
  [[nodiscard]] std::uint64_t seed() const noexcept override;
 private:
  std::uint64_t seed_;
  std::mt19937_64 engine_;
};

struct SyntheticStreamConfig final {
  std::uint64_t seed{};
  core::TimestampNs first_timestamp_ns{};
  core::DurationNs interval_ns{};
  std::size_t event_count{};
  core::OrderId first_order_id{1U};
  core::PriceTicks center_price_ticks{};
  core::QuantityLots quantity_lots{1U};
};

[[nodiscard]] std::vector<CanonicalEvent> generate_seeded_limit_events(const SyntheticStreamConfig& config);

struct SimulationConfig final {
  std::uint32_t event_schema_version{kEventSchemaVersion};
  std::string software_revision{"unknown"};
  std::string configuration_id{"default"};
  std::optional<std::uint64_t> random_seed{};
  bool enable_event_log{true};
};

struct RunMetadata final {
  std::uint32_t event_schema_version{};
  std::string software_revision;
  std::string configuration_id;
  std::string input_identity;
  std::optional<std::uint64_t> random_seed;
};
struct DispatchRecord final { CanonicalEvent event; bool accepted{}; exchange::RejectionReason rejection{exchange::RejectionReason::None}; std::size_t trades_created{}; };
struct SummaryStatistics final { std::size_t events_dispatched{}; std::size_t accepted_actions{}; std::size_t rejected_actions{}; std::size_t trades{}; core::QuantityLots traded_quantity{}; std::optional<core::PriceTicks> final_best_bid{}; std::optional<core::PriceTicks> final_best_ask{}; std::size_t final_live_orders{}; };
struct SimulationResult final { RunMetadata metadata; std::vector<DispatchRecord> dispatches; std::vector<exchange::Trade> trades; SummaryStatistics summary; };

class SimulationClock final {
 public:
  void advance_to(core::TimestampNs timestamp_ns);
  [[nodiscard]] std::optional<core::TimestampNs> now() const noexcept;
 private:
  std::optional<core::TimestampNs> now_;
};

class Simulator final {
 public:
  [[nodiscard]] SimulationResult run(const SimulationConfig& config, const EventSource& source);
  [[nodiscard]] static std::string event_log_ndjson(const SimulationResult& result);
 private:
  static void validate_config(const SimulationConfig& config);
  static void validate_events(const std::vector<CanonicalEvent>& events);
};

}  // namespace lob::simulation
