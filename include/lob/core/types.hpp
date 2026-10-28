#pragma once

#include <cstdint>
#include <stdexcept>

namespace lob::core {

using TimestampNs = std::int64_t;
using DurationNs = std::int64_t;
using PriceTicks = std::int64_t;
using QuantityLots = std::uint64_t;
using OrderId = std::uint64_t;
using TradeId = std::uint64_t;
using EventId = std::uint64_t;

/// Buy orders rest on the bid side; sell orders rest on the ask side.
enum class Side : std::uint8_t { Buy, Sell };

[[nodiscard]] constexpr bool is_valid_side(const Side side) noexcept {
  return side == Side::Buy || side == Side::Sell;
}

[[nodiscard]] constexpr Side opposite(const Side side) noexcept {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] constexpr bool is_valid_quantity(const QuantityLots quantity) noexcept {
  return quantity > 0U;
}

[[nodiscard]] constexpr bool is_valid_timestamp(const TimestampNs timestamp) noexcept {
  return timestamp >= 0;
}

void require_valid_timestamp(TimestampNs timestamp);
void require_valid_quantity(QuantityLots quantity);

}  // namespace lob::core
