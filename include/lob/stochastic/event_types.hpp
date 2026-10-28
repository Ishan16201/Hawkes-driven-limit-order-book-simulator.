#pragma once

/// Canonical event taxonomy for the six-dimensional Hawkes-driven order flow.
///
/// The same enum and indexing is used across:
///   - The Hawkes process (dimension indices)
///   - The exchange-event adapter (mapping to exchange actions)
///   - Event logs (recording)
///   - Calibration interfaces (parameter vectors)
///   - Analytics (counting and correlation)
///
/// These are event *categories*, not fully specified order instructions.
/// A sampled event type does not determine price, size, or cancel target;
/// those are resolved by configurable mark distributions.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lob::stochastic {

/// The six-dimensional order-flow event taxonomy from the design document.
///
/// Index assignments are part of the interface contract: changing them
/// invalidates calibrated parameter files. The ordering matches section 5
/// of docs/design.md.
enum class EventType : std::size_t {
  LimitBuy = 0,   ///< LB: limit addition on bid
  LimitSell = 1,  ///< LA: limit addition on ask
  CancelBuy = 2,  ///< CB: cancellation on bid
  CancelSell = 3, ///< CA: cancellation on ask
  MarketBuy = 4,  ///< MB: aggressive buy / marketable flow
  MarketSell = 5, ///< MS: aggressive sell / marketable flow
};

/// The number of event types in the taxonomy.
inline constexpr std::size_t kEventTypeCount = 6;

/// Human-readable short labels matching docs/design.md section 5.
inline constexpr std::array<std::string_view, kEventTypeCount> kEventTypeLabels = {
    "LB", "LA", "CB", "CA", "MB", "MS",
};

/// Convert a Hawkes dimension index to the typed enum.
/// Throws std::out_of_range if index >= kEventTypeCount.
[[nodiscard]] EventType event_type_from_index(std::size_t index);

/// Convert the typed enum to its Hawkes dimension index.
[[nodiscard]] constexpr std::size_t event_type_index(EventType type) noexcept {
  return static_cast<std::size_t>(type);
}

/// True if the event type is a cancellation category.
[[nodiscard]] constexpr bool is_cancellation(EventType type) noexcept {
  return type == EventType::CancelBuy || type == EventType::CancelSell;
}

/// True if the event type is a market order category.
[[nodiscard]] constexpr bool is_market_order(EventType type) noexcept {
  return type == EventType::MarketBuy || type == EventType::MarketSell;
}

/// True if the event type is a limit order category.
[[nodiscard]] constexpr bool is_limit_order(EventType type) noexcept {
  return type == EventType::LimitBuy || type == EventType::LimitSell;
}

/// True if the event type is a buy-side action.
[[nodiscard]] constexpr bool is_buy_side(EventType type) noexcept {
  return type == EventType::LimitBuy || type == EventType::CancelBuy ||
         type == EventType::MarketBuy;
}

}  // namespace lob::stochastic
