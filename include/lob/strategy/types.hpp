#pragma once

#include "lob/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lob::strategy {

enum class StrategyOrderStatus : std::uint8_t {
  New,
  Resting,
  PartiallyFilled,
  Filled,
  Cancelled,
  Expired,
  Rejected,
};

struct StrategyPriceLevel final {
  core::PriceTicks price_ticks{};
  core::QuantityLots quantity{};
  [[nodiscard]] bool operator==(const StrategyPriceLevel&) const = default;
};

/// Read-only market snapshot owned by the strategy API, not the exchange book.
struct StrategyBookSnapshot final {
  core::TimestampNs timestamp_ns{};
  std::vector<StrategyPriceLevel> bids{};
  std::vector<StrategyPriceLevel> asks{};
};

/// Public trade information intentionally omits exchange order identifiers.
struct StrategyMarketTrade final {
  core::TimestampNs timestamp_ns{};
  core::PriceTicks price_ticks{};
  core::QuantityLots quantity{};
  core::Side aggressor_side{core::Side::Buy};
};

/// Fee and commission schedule for strategy execution.
struct FeeSchedule final {
  /// Fee per lot executed as a passive maker (negative indicates liquidity rebate).
  double maker_fee_per_lot{0.0};

  /// Fee per lot executed as an aggressive taker (positive indicates fee).
  double taker_fee_per_lot{0.0};

  /// Fixed commission charged once on the order's first fill.
  double commission_per_order{0.0};
};

/// Risk limits enforced by the broker prior to exchange dispatch.
struct RiskLimits final {
  /// Maximum absolute net inventory in lots (|position| <= max_position_lots).
  core::QuantityLots max_position_lots{1000};

  /// Maximum size of any single order.
  core::QuantityLots max_order_lots{100};

  /// Whether short positions are permitted.
  bool allow_short_selling{true};

  /// Maximum permitted drawdown in currency units before triggering a risk halt.
  double max_drawdown_currency{1.0e9};
};

/// Latency model for simulating transmission delays.
struct LatencyModel final {
  /// Outbound latency: time from strategy decision to matching engine arrival.
  core::DurationNs outbound_delay_ns{0};

  /// Inbound latency: time from matching engine execution to strategy receipt.
  core::DurationNs inbound_delay_ns{0};
};

/// Type of strategy order command.
enum class StrategyOrderType : std::uint8_t {
  Limit,
  Market,
};

/// Command submitted by a strategy to the broker.
struct StrategyOrderCommand final {
  core::OrderId client_order_id{};
  core::TimestampNs timestamp_ns{};
  StrategyOrderType type{StrategyOrderType::Limit};
  core::Side side{core::Side::Buy};
  core::PriceTicks price_ticks{0};
  core::QuantityLots quantity_lots{0};
};

/// Reason for rejecting a strategy command.
enum class StrategyRejectionReason : std::uint8_t {
  None,
  InvalidOrder,
  PositionLimitExceeded,
  OrderSizeLimitExceeded,
  ShortSellingNotAllowed,
  RiskHaltActive,
  ExchangeRejection,
  UnknownOrder,
};

/// Execution report received by a strategy.
struct StrategyExecutionReport final {
  core::OrderId client_order_id{};
  core::TimestampNs timestamp_ns{};
  core::Side side{core::Side::Buy};
  StrategyOrderType order_type{StrategyOrderType::Limit};
  core::PriceTicks execution_price_ticks{0};
  core::QuantityLots filled_quantity{0};
  core::QuantityLots remaining_quantity{0};
  double fee_paid_currency{0.0};
  /// Signed fill cost versus the pre-execution midprice; not an extra cash charge.
  double execution_cost_currency{0.0};
  bool is_passive{false};
  StrategyOrderStatus status{StrategyOrderStatus::New};
  StrategyRejectionReason rejection{StrategyRejectionReason::None};
};

/// Open resting order tracked by the broker.
struct OpenStrategyOrder final {
  core::OrderId client_order_id{};
  core::TimestampNs submission_time_ns{};
  core::Side side{core::Side::Buy};
  core::PriceTicks price_ticks{0};
  core::QuantityLots initial_quantity{0};
  core::QuantityLots remaining_quantity{0};
};

/// Complete portfolio accounting state at a point in time.
struct PortfolioState final {
  /// Net inventory position: positive = long, negative = short, 0 = flat.
  std::int64_t position_lots{0};

  /// Liquid cash balance in currency units.
  double cash_currency{0.0};

  /// Volume-weighted average entry price in ticks of the current open position.
  double average_entry_price_ticks{0.0};

  /// Cumulative realized trading profit/loss before fees.
  double gross_realized_pnl_currency{0.0};

  /// Mark-to-market unrealized profit/loss on the open inventory.
  double unrealized_pnl_currency{0.0};

  /// Total cumulative fees and commissions paid.
  double total_fees_currency{0.0};

  /// Total gross traded turnover in currency units.
  double gross_turnover_currency{0.0};

  /// Peak equity seen during the run (for drawdown calculation).
  double peak_equity_currency{0.0};

  /// Cash plus the latest marked inventory value; fees are already reflected in cash.
  double marked_equity_currency{0.0};

  /// Maximum peak-to-trough drawdown experienced.
  double max_drawdown_currency{0.0};

  /// Net account equity relative to zero initial cash.
  [[nodiscard]] double total_equity_currency() const noexcept {
    return marked_equity_currency;
  }

  /// Net realized P&L = Gross Realized P&L - Fees.
  [[nodiscard]] double net_realized_pnl_currency() const noexcept {
    return gross_realized_pnl_currency - total_fees_currency;
  }

  /// Net total P&L = Gross Realized P&L + Unrealized P&L - Fees.
  [[nodiscard]] double net_total_pnl_currency() const noexcept {
    return gross_realized_pnl_currency + unrealized_pnl_currency - total_fees_currency;
  }
};

/// Metric results for evaluating parent-order execution strategies (e.g. TWAP, Market).
struct ParentOrderExecutionMetrics final {
  core::QuantityLots target_quantity{0};
  core::QuantityLots executed_quantity{0};
  double completion_rate{0.0};  // executed / target

  double arrival_midprice_ticks{0.0};
  double execution_vwap_ticks{0.0};

  /// Implementation shortfall in ticks = Side * (VWAP - ArrivalMidprice).
  double implementation_shortfall_ticks{0.0};

  /// Implementation shortfall in currency units.
  double implementation_shortfall_currency{0.0};

  /// Total execution costs including fees.
  double total_cost_currency{0.0};
};

/// Metric results for evaluating continuous inventory / market-making strategies.
struct MarketMakingMetrics final {
  double gross_realized_pnl{0.0};
  double gross_total_pnl{0.0};
  double net_realized_pnl{0.0};
  double unrealized_pnl{0.0};
  double total_net_pnl{0.0};
  double total_fees{0.0};
  double execution_cost_currency{0.0};
  double max_drawdown{0.0};
  double gross_turnover{0.0};
  std::size_t total_trades{0};
  std::size_t passive_fills{0};
  std::size_t aggressive_fills{0};
  double passive_fill_ratio{0.0};  // passive_fills / total_trades
  double order_fill_rate{0.0};     // distinct strategy orders with fills / submitted orders
  double max_inventory_exposure{0.0};
};

/// Complete evaluation report for a strategy run.
struct StrategyEvaluationReport final {
  std::string strategy_name{"unknown"};
  std::uint64_t seed{0};
  double duration_seconds{0.0};

  PortfolioState final_portfolio;
  MarketMakingMetrics market_making;
  std::optional<ParentOrderExecutionMetrics> parent_order;

  std::vector<StrategyExecutionReport> execution_history;
  std::vector<OpenStrategyOrder> final_open_orders;

  [[nodiscard]] std::string to_formatted_report() const;
  [[nodiscard]] std::string to_json() const;
};

}  // namespace lob::strategy
