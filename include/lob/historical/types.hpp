#pragma once

#include "lob/core/types.hpp"
#include "lob/stochastic/event_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lob::historical {

inline constexpr std::uint32_t kHistoricalSchemaVersion = 1U;

/// Raw feed message categories.
enum class RawFeedEventType : std::uint8_t {
  OrderSubmit,     ///< Submission of a limit order
  OrderExecution,  ///< Trade execution / fill
  OrderCancel,     ///< Order cancellation
  OrderReplace,    ///< Order replacement / amendment
  FeedStatus,      ///< Feed status / heartbeat / trading halt / sync marker
  Unknown,
};

[[nodiscard]] std::string raw_event_type_to_string(RawFeedEventType type);
[[nodiscard]] RawFeedEventType raw_event_type_from_string(const std::string& str);

/// A raw event record as ingested from external feeds or historical files.
struct RawEventRecord final {
  std::uint32_t schema_version{kHistoricalSchemaVersion};
  core::TimestampNs timestamp_ns{};
  std::uint64_t source_sequence{0};
  RawFeedEventType event_type{RawFeedEventType::Unknown};
  core::Side side{core::Side::Buy};
  core::PriceTicks price_ticks{0};
  core::QuantityLots quantity_lots{0};
  std::optional<core::OrderId> order_id{};
  std::optional<core::TradeId> trade_id{};
  std::string raw_message_type{};
};

/// Policy for mapping replacements and preventing duplicate action counts.
enum class ReplaceHandlingPolicy : std::uint8_t {
  /// Map replace as cancellation followed by new submission.
  DecomposeToCancelAndSubmit,
  /// Ignore replacement for Hawkes intensity flow (record in audit log only).
  AuditOnly,
};

/// Configuration for mapping raw feed events into canonical Hawkes event taxonomy.
struct EventMappingConfig final {
  /// Handling of replace/modify messages.
  ReplaceHandlingPolicy replace_policy{ReplaceHandlingPolicy::DecomposeToCancelAndSubmit};

  /// If true, suppress trade prints when an order execution was already ingested.
  bool deduplicate_executions{true};

  /// Disallow negative timestamps or zero timestamps if require_positive_time is true.
  bool require_positive_timestamps{false};
};

/// Summary report from validating a historical dataset.
struct IngestionValidationReport final {
  std::string source_identity{"unknown"};
  std::size_t total_records_read{0};
  std::size_t valid_canonical_events{0};
  std::size_t malformed_records{0};
  std::size_t timestamp_order_violations{0};
  std::size_t duplicate_sequences{0};
  std::size_t feed_status_messages_skipped{0};

  bool has_order_level_data{true};
  bool has_trade_level_data{true};

  core::TimestampNs min_timestamp_ns{0};
  core::TimestampNs max_timestamp_ns{0};

  std::vector<std::string> validation_errors;
  std::vector<std::string> validation_warnings;

  [[nodiscard]] bool is_valid() const noexcept {
    return malformed_records == 0 && timestamp_order_violations == 0 && valid_canonical_events > 0;
  }

  [[nodiscard]] std::string summary_text() const;
};

/// Canonical mapped event ready for simulation replay or Hawkes estimation.
struct MappedHistoricalEvent final {
  core::TimestampNs timestamp_ns{};
  std::uint64_t sequence{0};
  stochastic::EventType hawkes_type{stochastic::EventType::LimitBuy};
  core::PriceTicks price_ticks{0};
  core::QuantityLots quantity_lots{0};
  std::optional<core::OrderId> order_id{};
  std::optional<core::TradeId> trade_id{};
};

}  // namespace lob::historical
