#pragma once

#include "lob/core/types.hpp"
#include "lob/exchange/matching_engine.hpp"
#include "lob/strategy/strategy.hpp"
#include "lob/strategy/types.hpp"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lob::strategy {

/// Mediates strategy commands, enforces risk limits, applies latency,
/// routes to the matching engine, and executes event-driven portfolio accounting.
class OrderBroker final : public StrategyActionEmitter {
 public:
  struct Config {
    double currency_per_tick{1.0};
    FeeSchedule fees{};
    RiskLimits risk{};
    LatencyModel latency{};
  };

  OrderBroker();
  explicit OrderBroker(Config config);

  // --- StrategyActionEmitter implementation ---
  void submit_limit(core::OrderId client_order_id,
                    core::Side side,
                    core::PriceTicks price_ticks,
                    core::QuantityLots quantity) override;

  void submit_market(core::OrderId client_order_id,
                     core::Side side,
                     core::QuantityLots quantity) override;

  void cancel(core::OrderId client_order_id) override;

  // --- Exchange integration & Accounting ---

  /// Dispatches pending queued strategy commands whose latency has matured to the exchange.
  void process_pending_commands(exchange::MatchingEngine& engine,
                                core::TimestampNs current_time_ns,
                                Strategy& strategy,
                                const StrategyContext& ctx);

  /// Delivers execution reports whose inbound latency has elapsed.
  void process_pending_reports(core::TimestampNs current_time_ns,
                               Strategy& strategy,
                               const StrategyContext& ctx);

  /// Notifies the broker that an exchange-side cancellation closed an owned order.
  void process_external_cancellation(core::OrderId client_order_id,
                                     core::TimestampNs timestamp_ns);

  [[nodiscard]] std::optional<core::TimestampNs> next_command_time() const noexcept;
  [[nodiscard]] std::optional<core::TimestampNs> next_report_time() const noexcept;

  /// Called when an external market event updates the midprice, for mark-to-market.
  void mark_to_market(std::optional<double> midprice_ticks);
  void set_execution_reference_midprice(std::optional<double> midprice_ticks);

  /// Checks if an external trade matched one of our resting orders, and if so applies a passive fill.
  void process_trade(const exchange::Trade& trade,
                     Strategy& strategy,
                     const StrategyContext& ctx);

  /// Sets current time for timestamping newly submitted commands.
  void set_current_time(core::TimestampNs current_time_ns);

  /// Access current portfolio state.
  [[nodiscard]] const PortfolioState& portfolio() const noexcept { return portfolio_; }

  /// Access open strategy orders.
  [[nodiscard]] const std::vector<OpenStrategyOrder>& open_orders() const noexcept {
    return open_orders_list_;
  }

  /// Complete report ledger; timestamps are scheduled strategy receipt times.
  [[nodiscard]] const std::vector<StrategyExecutionReport>& execution_history() const noexcept {
    return execution_history_;
  }

  /// Total submitted commands.
  [[nodiscard]] std::size_t commands_submitted() const noexcept { return commands_submitted_; }
  [[nodiscard]] std::size_t commands_rejected() const noexcept { return commands_rejected_; }
  [[nodiscard]] std::size_t total_trades() const noexcept { return total_trades_; }
  [[nodiscard]] std::size_t passive_fills() const noexcept { return passive_fills_; }
  [[nodiscard]] std::size_t aggressive_fills() const noexcept { return aggressive_fills_; }
  [[nodiscard]] std::size_t submitted_orders() const noexcept { return submitted_order_ids_.size(); }
  [[nodiscard]] std::size_t filled_orders() const noexcept { return filled_order_ids_.size(); }
  [[nodiscard]] double execution_cost_currency() const noexcept {
    return execution_cost_currency_;
  }
  [[nodiscard]] std::size_t max_inventory_exposure_lots() const noexcept {
    return max_inventory_exposure_lots_;
  }

 private:
  struct QueuedCommand {
    core::TimestampNs execute_time_ns{0};
    StrategyOrderCommand command{};
    bool is_cancel{false};
  };

  struct QueuedReport {
    core::TimestampNs receipt_time_ns{0};
    StrategyExecutionReport report{};
  };

  [[nodiscard]] bool would_breach_position_limit(core::Side side,
                                                  core::QuantityLots quantity) const noexcept;
  [[nodiscard]] bool would_breach_short_selling(core::Side side,
                                                 core::QuantityLots quantity) const noexcept;
  [[nodiscard]] bool risk_halt_active() const noexcept;
  void queue_execution_report(StrategyExecutionReport report);
  void apply_fill(const exchange::Trade& trade,
                  bool is_passive,
                  core::OrderId client_order_id,
                  core::Side side,
                  StrategyOrderType order_type);

  void handle_order_closed(core::OrderId client_order_id);

  Config config_;
  PortfolioState portfolio_;
  std::optional<double> last_mark_price_ticks_{};
  std::optional<double> execution_reference_midprice_ticks_{};
  core::TimestampNs current_time_ns_{0};
  std::vector<QueuedCommand> pending_commands_;
  std::vector<QueuedReport> pending_reports_;
  std::vector<OpenStrategyOrder> open_orders_list_;
  std::unordered_map<core::OrderId, OpenStrategyOrder> open_orders_map_;
  std::unordered_set<core::OrderId> commissioned_order_ids_;
  std::unordered_set<core::OrderId> submitted_order_ids_;
  std::unordered_set<core::OrderId> filled_order_ids_;
  std::vector<StrategyExecutionReport> execution_history_;

  std::size_t commands_submitted_{0};
  std::size_t commands_rejected_{0};
  std::size_t total_trades_{0};
  std::size_t passive_fills_{0};
  std::size_t aggressive_fills_{0};
  double execution_cost_currency_{0.0};
  std::size_t max_inventory_exposure_lots_{0};
};

}  // namespace lob::strategy
