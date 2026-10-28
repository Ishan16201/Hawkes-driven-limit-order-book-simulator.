#pragma once

#include "lob/core/types.hpp"
#include "lob/strategy/types.hpp"

#include <optional>
#include <vector>

namespace lob::strategy {

/// Read-only causal view of the market and portfolio presented to a strategy.
///
/// CRITICAL INVARIANT:
/// This context exposes ONLY information strictly available at current_time_ns().
/// It contains NO future events, NO future book states, NO future trades, and NO RNG access.
class StrategyContext {
 public:
  virtual ~StrategyContext() = default;

  /// Current simulated time in nanoseconds.
  [[nodiscard]] virtual core::TimestampNs current_time_ns() const noexcept = 0;

  /// Current best bid price (nullopt if bid side empty).
  [[nodiscard]] virtual std::optional<core::PriceTicks> best_bid() const noexcept = 0;

  /// Current best ask price (nullopt if ask side empty).
  [[nodiscard]] virtual std::optional<core::PriceTicks> best_ask() const noexcept = 0;

  /// Current midprice in ticks (nullopt if either side empty).
  [[nodiscard]] virtual std::optional<double> midprice() const noexcept = 0;

  /// Top price levels of the central order book as of current_time_ns().
  [[nodiscard]] virtual const StrategyBookSnapshot& current_book() const = 0;

  /// Current portfolio accounting state (cash, position, entry price, PnL).
  [[nodiscard]] virtual const PortfolioState& portfolio() const noexcept = 0;

  /// Currently open resting orders submitted by this strategy.
  [[nodiscard]] virtual const std::vector<OpenStrategyOrder>& open_orders() const noexcept = 0;
};

/// Interface for submitting strategy actions to the broker.
class StrategyActionEmitter {
 public:
  virtual ~StrategyActionEmitter() = default;

  /// Submits a resting limit order.
  virtual void submit_limit(core::OrderId client_order_id,
                            core::Side side,
                            core::PriceTicks price_ticks,
                            core::QuantityLots quantity) = 0;

  /// Submits an aggressive market order.
  virtual void submit_market(core::OrderId client_order_id,
                             core::Side side,
                             core::QuantityLots quantity) = 0;

  /// Requests cancellation of an open order by client order ID.
  virtual void cancel(core::OrderId client_order_id) = 0;
};

/// Abstract base class for all algorithmic execution and trading strategies.
class Strategy {
 public:
  virtual ~Strategy() = default;

  /// Called when the strategy environment starts.
  virtual void on_start(const StrategyContext& ctx, StrategyActionEmitter& emitter) = 0;

  /// Called when a market quote / book change occurs.
  virtual void on_quote(const StrategyContext& ctx, StrategyActionEmitter& emitter) = 0;

  /// Called when any public trade occurs in the market.
  virtual void on_trade(const StrategyContext& ctx,
                        const StrategyMarketTrade& trade,
                        StrategyActionEmitter& emitter) = 0;

  /// Called when an execution report or fill is received for this strategy's order.
  virtual void on_execution_report(const StrategyContext& ctx,
                                   const StrategyExecutionReport& report,
                                   StrategyActionEmitter& emitter) = 0;

  /// Called on scheduled timer ticks (e.g. for TWAP or periodic rebalancing).
  virtual void on_timer(const StrategyContext& ctx, StrategyActionEmitter& emitter) = 0;

  /// Called when the simulation reaches its horizon.
  virtual void on_stop(const StrategyContext& ctx, StrategyActionEmitter& emitter) = 0;

  /// Strategy identifier / name.
  [[nodiscard]] virtual std::string name() const = 0;
};

}  // namespace lob::strategy
