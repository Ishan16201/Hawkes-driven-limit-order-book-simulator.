#include "lob/strategy/strategy_evaluator.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>

namespace lob::strategy {
namespace {

core::TimestampNs seconds_to_ns(const double seconds) noexcept {
  return static_cast<core::TimestampNs>(std::floor(seconds * 1.0e9));
}

core::QuantityLots draw_size(
    const core::QuantityLots lo, const core::QuantityLots hi,
    std::mt19937_64& rng) {
  std::uniform_int_distribution<core::QuantityLots> dist(lo, hi);
  return dist(rng);
}

core::PriceTicks draw_offset(
    const core::PriceTicks min_offset, const core::PriceTicks max_offset,
    std::mt19937_64& rng) {
  if (min_offset == max_offset) return min_offset;
  std::uniform_int_distribution<core::PriceTicks> dist(min_offset, max_offset);
  return dist(rng);
}

core::PriceTicks compute_limit_buy_price(
    const exchange::OrderBook& book,
    const stochastic::PricePlacementConfig& config,
    std::mt19937_64& rng) {
  const auto anchor = book.best_bid().value_or(config.reference_price_ticks);
  const auto offset = draw_offset(config.min_offset_ticks, config.max_offset_ticks, rng);
  const auto price = anchor - offset;
  return price > 0 ? price : 1;
}

core::PriceTicks compute_limit_sell_price(
    const exchange::OrderBook& book,
    const stochastic::PricePlacementConfig& config,
    std::mt19937_64& rng) {
  const auto anchor = book.best_ask().value_or(config.reference_price_ticks);
  const auto offset = draw_offset(config.min_offset_ticks, config.max_offset_ticks, rng);
  return anchor + offset;
}

std::optional<core::OrderId> select_cancel_target(
    const exchange::OrderBook& book,
    const core::Side side,
    const stochastic::CancelTargetPolicy policy,
    std::mt19937_64& rng) {
  const auto best_price = (side == core::Side::Buy) ? book.best_bid() : book.best_ask();
  if (!best_price.has_value()) return std::nullopt;

  switch (policy) {
    case stochastic::CancelTargetPolicy::BestLevelFront: {
      const auto ids = book.order_ids_at(side, *best_price);
      if (ids.empty()) return std::nullopt;
      return ids.front();
    }
    case stochastic::CancelTargetPolicy::UniformRandom: {
      const auto ids = book.order_ids_at(side, *best_price);
      if (ids.empty()) return std::nullopt;
      std::uniform_int_distribution<std::size_t> dist(0, ids.size() - 1);
      return ids[dist(rng)];
    }
  }
  return std::nullopt;
}

std::optional<double> calculate_midprice(const exchange::OrderBook& book) noexcept {
  const auto bb = book.best_bid();
  const auto ba = book.best_ask();
  if (bb.has_value() && ba.has_value()) {
    return (static_cast<double>(*bb) + static_cast<double>(*ba)) * 0.5;
  }
  return std::nullopt;
}

/// Strictly causal StrategyContext implementation.
/// Exposes ONLY information up to current_time_ns(). Future market state is unreachable.
class StrategyContextImpl final : public StrategyContext {
 public:
  StrategyContextImpl(const exchange::MatchingEngine& engine,
                      const OrderBroker& broker,
                      const core::TimestampNs current_time)
      : engine_(engine), broker_(broker), current_time_(current_time) {}

  void update_time(const core::TimestampNs t) noexcept {
    current_time_ = t;
    cached_snapshot_.reset();
  }

  [[nodiscard]] core::TimestampNs current_time_ns() const noexcept override {
    return current_time_;
  }

  [[nodiscard]] std::optional<core::PriceTicks> best_bid() const noexcept override {
    return engine_.book().best_bid();
  }

  [[nodiscard]] std::optional<core::PriceTicks> best_ask() const noexcept override {
    return engine_.book().best_ask();
  }

  [[nodiscard]] std::optional<double> midprice() const noexcept override {
    return calculate_midprice(engine_.book());
  }

  [[nodiscard]] const StrategyBookSnapshot& current_book() const override {
    if (!cached_snapshot_.has_value() || cached_snapshot_time_ != current_time_) {
      const auto exchange_snapshot = engine_.snapshot(current_time_);
      StrategyBookSnapshot strategy_snapshot;
      strategy_snapshot.timestamp_ns = exchange_snapshot.timestamp_ns;
      strategy_snapshot.bids.reserve(exchange_snapshot.bids.size());
      strategy_snapshot.asks.reserve(exchange_snapshot.asks.size());
      for (const auto& level : exchange_snapshot.bids) {
        strategy_snapshot.bids.push_back({level.price_ticks, level.quantity});
      }
      for (const auto& level : exchange_snapshot.asks) {
        strategy_snapshot.asks.push_back({level.price_ticks, level.quantity});
      }
      cached_snapshot_ = std::move(strategy_snapshot);
      cached_snapshot_time_ = current_time_;
    }
    return *cached_snapshot_;
  }

  [[nodiscard]] const PortfolioState& portfolio() const noexcept override {
    return broker_.portfolio();
  }

  [[nodiscard]] const std::vector<OpenStrategyOrder>& open_orders() const noexcept override {
    return broker_.open_orders();
  }

 private:
  const exchange::MatchingEngine& engine_;
  const OrderBroker& broker_;
  core::TimestampNs current_time_{0};
  mutable std::optional<StrategyBookSnapshot> cached_snapshot_{};
  mutable core::TimestampNs cached_snapshot_time_{0};
};

struct TimelineItem {
  core::TimestampNs timestamp_ns;
  bool is_timer{false};
  std::size_t hawkes_index{0};

  bool operator<(const TimelineItem& other) const noexcept {
    if (timestamp_ns != other.timestamp_ns) {
      return timestamp_ns < other.timestamp_ns;
    }
    if (is_timer != other.is_timer) {
      return is_timer;
    }
    return !is_timer && hawkes_index < other.hawkes_index;
  }
};

}  // namespace

StrategyEvaluationReport StrategyEvaluator::run_single(
    Strategy& strategy,
    const stochastic::SyntheticMarketConfig& market_config,
    const StrategyEvaluatorConfig& eval_config) {
  if (eval_config.timer_interval_ns < 0 ||
      (eval_config.evaluate_parent_order &&
       (eval_config.parent_order_target == 0 ||
        !core::is_valid_side(eval_config.parent_order_side)))) {
    throw std::invalid_argument{"invalid strategy evaluation configuration"};
  }

  exchange::MatchingEngine engine;
  std::mt19937_64 marks_rng(market_config.seed ^ 0x9e3779b97f4a7c15ULL);

  // Construct initial book at t = 0
  const auto initial_orders =
      stochastic::HawkesMarketAdapter::generate_initial_book(market_config.initial_book);
  core::OrderId next_hawkes_order_id = 1;
  for (const auto& order : initial_orders) {
    const auto res = engine.submit_limit(order.id, 0, order.side, order.price_ticks, order.quantity);
    if (!res.accepted) {
      throw std::runtime_error{"Initial book order rejected in strategy evaluation"};
    }
    if (order.id >= next_hawkes_order_id) {
      next_hawkes_order_id = order.id + 1;
    }
  }

  const auto arrival_midprice = calculate_midprice(engine.book());
  OrderBroker broker(eval_config.broker_config);
  StrategyContextImpl ctx(engine, broker, 0);

  // Strategy starts at t = 0
  broker.set_current_time(0);
  strategy.on_start(ctx, broker);
  const auto process_due_events = [&](const core::TimestampNs up_to_time_ns) {
    while (true) {
      const auto command_time = broker.next_command_time();
      const auto report_time = broker.next_report_time();
      const bool command_due = command_time.has_value() && *command_time <= up_to_time_ns;
      const bool report_due = report_time.has_value() && *report_time <= up_to_time_ns;
      if (!command_due && !report_due) break;

      const auto next_time = command_due && report_due
                                 ? std::min(*command_time, *report_time)
                                 : (command_due ? *command_time : *report_time);
      ctx.update_time(next_time);
      broker.set_current_time(next_time);
      broker.process_pending_reports(next_time, strategy, ctx);
      broker.process_pending_commands(engine, next_time, strategy, ctx);
      broker.mark_to_market(calculate_midprice(engine.book()));
    }
    ctx.update_time(up_to_time_ns);
    broker.set_current_time(up_to_time_ns);
  };
  process_due_events(0);
  broker.mark_to_market(calculate_midprice(engine.book()));

  // Generate Hawkes process arrivals
  stochastic::ExponentialHawkesProcess hawkes(market_config.hawkes, market_config.seed);
  const auto hawkes_events = hawkes.simulate_until(market_config.horizon_seconds);

  const core::TimestampNs horizon_ns =
      static_cast<core::TimestampNs>(std::floor(market_config.horizon_seconds * 1.0e9));

  // Build merged chronological timeline
  std::vector<TimelineItem> timeline;
  timeline.reserve(hawkes_events.size() + 100);

  for (std::size_t i = 0; i < hawkes_events.size(); ++i) {
    const auto t_ns = seconds_to_ns(hawkes_events[i].time_seconds);
    if (t_ns <= horizon_ns) {
      timeline.push_back(TimelineItem{
          .timestamp_ns = t_ns,
          .is_timer = false,
          .hawkes_index = i,
      });
    }
  }

  if (eval_config.timer_interval_ns > 0) {
    for (core::TimestampNs t = eval_config.timer_interval_ns; t <= horizon_ns;
         t += eval_config.timer_interval_ns) {
      timeline.push_back(TimelineItem{
          .timestamp_ns = t,
          .is_timer = true,
          .hawkes_index = 0,
      });
    }
  }

  std::sort(timeline.begin(), timeline.end());

  // Execution loop
  for (const auto& item : timeline) {
    process_due_events(item.timestamp_ns);

    if (item.is_timer) {
      strategy.on_timer(ctx, broker);
      process_due_events(item.timestamp_ns);
    } else {
      const auto& he = hawkes_events[item.hawkes_index];
      const auto event_type = stochastic::event_type_from_index(he.type);
      const auto time_ns = item.timestamp_ns;
      const auto trades_before = engine.trades().size();
      broker.set_execution_reference_midprice(calculate_midprice(engine.book()));

      if (stochastic::is_cancellation(event_type)) {
        const auto side = stochastic::is_buy_side(event_type) ? core::Side::Buy : core::Side::Sell;
        const auto target = select_cancel_target(
            engine.book(), side, market_config.marks.cancellation.policy, marks_rng);
        if (target.has_value()) {
          const auto cancelled = engine.cancel(*target, time_ns);
          if (cancelled.accepted) {
            broker.process_external_cancellation(*target, time_ns);
          }
        }
      } else if (stochastic::is_market_order(event_type)) {
        const auto side = stochastic::is_buy_side(event_type) ? core::Side::Buy : core::Side::Sell;
        const auto size = draw_size(
            market_config.marks.market_size.min_lots,
            market_config.marks.market_size.max_lots, marks_rng);
        const auto oid = next_hawkes_order_id++;
        static_cast<void>(engine.submit_market(oid, time_ns, side, size));
      } else {
        const auto is_buy = stochastic::is_buy_side(event_type);
        const auto side = is_buy ? core::Side::Buy : core::Side::Sell;
        const auto size = draw_size(
            market_config.marks.limit_size.min_lots,
            market_config.marks.limit_size.max_lots, marks_rng);
        const auto price = is_buy ? compute_limit_buy_price(
                                        engine.book(), market_config.marks.price_placement, marks_rng)
                                  : compute_limit_sell_price(
                                        engine.book(), market_config.marks.price_placement, marks_rng);
        const auto oid = next_hawkes_order_id++;
        static_cast<void>(engine.submit_limit(oid, time_ns, side, price, size));
      }

      // Check for passive strategy fills or trades
      const auto trades_after = engine.trades().size();
      for (std::size_t tr_idx = trades_before; tr_idx < trades_after; ++tr_idx) {
        const auto& tr = engine.trades()[tr_idx];
        broker.process_trade(tr, strategy, ctx);
      }
      broker.mark_to_market(calculate_midprice(engine.book()));
      process_due_events(time_ns);
      for (std::size_t tr_idx = trades_before; tr_idx < trades_after; ++tr_idx) {
        const auto& tr = engine.trades()[tr_idx];
        strategy.on_trade(ctx,
                          StrategyMarketTrade{
                              .timestamp_ns = tr.timestamp_ns,
                              .price_ticks = tr.price_ticks,
                              .quantity = tr.quantity,
                              .aggressor_side = tr.aggressor_side,
                          },
                          broker);
      }

      strategy.on_quote(ctx, broker);
      process_due_events(time_ns);
    }
  }

  // Horizon reached: on_stop
  process_due_events(horizon_ns);
  strategy.on_stop(ctx, broker);
  process_due_events(horizon_ns);
  broker.mark_to_market(calculate_midprice(engine.book()));

  // Assemble report
  StrategyEvaluationReport report;
  report.strategy_name = strategy.name();
  report.seed = market_config.seed;
  report.duration_seconds = market_config.horizon_seconds;
  report.final_portfolio = broker.portfolio();
  report.execution_history = broker.execution_history();
  report.final_open_orders = broker.open_orders();

  report.market_making.gross_realized_pnl = broker.portfolio().gross_realized_pnl_currency;
  report.market_making.gross_total_pnl =
      broker.portfolio().gross_realized_pnl_currency +
      broker.portfolio().unrealized_pnl_currency;
  report.market_making.net_realized_pnl = broker.portfolio().net_realized_pnl_currency();
  report.market_making.unrealized_pnl = broker.portfolio().unrealized_pnl_currency;
  report.market_making.total_net_pnl = broker.portfolio().net_total_pnl_currency();
  report.market_making.total_fees = broker.portfolio().total_fees_currency;
  report.market_making.execution_cost_currency = broker.execution_cost_currency();
  report.market_making.max_drawdown = broker.portfolio().max_drawdown_currency;
  report.market_making.gross_turnover = broker.portfolio().gross_turnover_currency;
  report.market_making.total_trades = broker.total_trades();
  report.market_making.passive_fills = broker.passive_fills();
  report.market_making.aggressive_fills = broker.aggressive_fills();
  report.market_making.passive_fill_ratio =
      broker.total_trades() > 0
          ? static_cast<double>(broker.passive_fills()) / static_cast<double>(broker.total_trades())
          : 0.0;
  report.market_making.order_fill_rate =
      broker.submitted_orders() > 0
          ? static_cast<double>(broker.filled_orders()) /
                static_cast<double>(broker.submitted_orders())
          : 0.0;
  report.market_making.max_inventory_exposure =
      static_cast<double>(broker.max_inventory_exposure_lots());

  if (eval_config.evaluate_parent_order) {
    ParentOrderExecutionMetrics pm;
    pm.target_quantity = eval_config.parent_order_target;

    core::QuantityLots exec_qty = 0;
    double total_cost_ticks = 0.0;
    double parent_fees_currency = 0.0;
    for (const auto& rep : report.execution_history) {
      if (rep.filled_quantity > 0 && rep.side == eval_config.parent_order_side) {
        exec_qty += rep.filled_quantity;
        total_cost_ticks += static_cast<double>(rep.filled_quantity) *
                            static_cast<double>(rep.execution_price_ticks);
        parent_fees_currency += rep.fee_paid_currency;
      }
    }
    pm.executed_quantity = exec_qty;
    pm.completion_rate = pm.target_quantity > 0
                             ? static_cast<double>(exec_qty) / static_cast<double>(pm.target_quantity)
                             : 0.0;
    pm.arrival_midprice_ticks = arrival_midprice.value_or(0.0);
    pm.execution_vwap_ticks = exec_qty > 0 ? (total_cost_ticks / static_cast<double>(exec_qty)) : 0.0;

    const double is_sign = (eval_config.parent_order_side == core::Side::Buy) ? 1.0 : -1.0;
    pm.implementation_shortfall_ticks = is_sign * (pm.execution_vwap_ticks - pm.arrival_midprice_ticks);
    pm.implementation_shortfall_currency =
        pm.implementation_shortfall_ticks * static_cast<double>(exec_qty) *
        eval_config.broker_config.currency_per_tick;
    pm.total_cost_currency =
        pm.implementation_shortfall_currency + parent_fees_currency;

    report.parent_order = pm;
  }

  return report;
}

MultiRunEvaluationSummary StrategyEvaluator::run_multiple(
    std::function<std::unique_ptr<Strategy>()> strategy_factory,
    stochastic::SyntheticMarketConfig base_market_config,
    const std::vector<std::uint64_t>& seeds,
    const StrategyEvaluatorConfig& eval_config) {
  if (seeds.empty() || !strategy_factory) {
    throw std::invalid_argument{"multi-run evaluation requires seeds and a strategy factory"};
  }

  MultiRunEvaluationSummary summary;
  summary.num_runs = seeds.size();
  summary.parent_order_evaluation = eval_config.evaluate_parent_order;
  summary.individual_runs.reserve(seeds.size());

  std::vector<double> net_pnls;
  std::vector<double> gross_pnls;
  std::vector<double> turnovers;
  std::vector<double> fees;
  std::vector<double> execution_costs;
  std::vector<double> drawdowns;
  std::vector<double> inventory_exposures;
  std::vector<double> order_fill_rates;
  std::vector<double> passive_fill_ratios;
  std::vector<double> completion_rates;
  std::vector<double> shortfalls;
  std::vector<double> parent_total_costs;

  net_pnls.reserve(seeds.size());
  gross_pnls.reserve(seeds.size());
  turnovers.reserve(seeds.size());
  fees.reserve(seeds.size());
  execution_costs.reserve(seeds.size());
  drawdowns.reserve(seeds.size());
  inventory_exposures.reserve(seeds.size());
  order_fill_rates.reserve(seeds.size());
  passive_fill_ratios.reserve(seeds.size());
  if (eval_config.evaluate_parent_order) {
    completion_rates.reserve(seeds.size());
    shortfalls.reserve(seeds.size());
    parent_total_costs.reserve(seeds.size());
  }

  for (const auto seed : seeds) {
    base_market_config.seed = seed;
    auto strat = strategy_factory();
    if (!strat) {
      throw std::runtime_error{"strategy factory returned no strategy"};
    }
    summary.strategy_name = strat->name();

    auto report = run_single(*strat, base_market_config, eval_config);
    net_pnls.push_back(report.final_portfolio.net_total_pnl_currency());
    gross_pnls.push_back(report.market_making.gross_total_pnl);
    turnovers.push_back(report.final_portfolio.gross_turnover_currency);
    fees.push_back(report.final_portfolio.total_fees_currency);
    execution_costs.push_back(report.market_making.execution_cost_currency);
    drawdowns.push_back(report.final_portfolio.max_drawdown_currency);
    inventory_exposures.push_back(report.market_making.max_inventory_exposure);
    order_fill_rates.push_back(report.market_making.order_fill_rate);
    passive_fill_ratios.push_back(report.market_making.passive_fill_ratio);

    if (report.parent_order.has_value()) {
      completion_rates.push_back(report.parent_order->completion_rate);
      shortfalls.push_back(report.parent_order->implementation_shortfall_currency);
      parent_total_costs.push_back(report.parent_order->total_cost_currency);
    }

    summary.individual_runs.push_back(std::move(report));
  }

  const double n = static_cast<double>(seeds.size());
  auto calc_mean = [n](const std::vector<double>& v) {
    return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / n;
  };

  auto calc_std = [](const std::vector<double>& v, const double mean) {
    if (v.size() <= 1) return 0.0;
    double sum_sq = 0.0;
    for (const auto x : v) {
      const double diff = x - mean;
      sum_sq += diff * diff;
    }
    return std::sqrt(sum_sq / static_cast<double>(v.size() - 1));
  };

  summary.mean_net_total_pnl = calc_mean(net_pnls);
  summary.std_net_total_pnl = calc_std(net_pnls, summary.mean_net_total_pnl);
  summary.se_net_total_pnl = (n > 0) ? (summary.std_net_total_pnl / std::sqrt(n)) : 0.0;
  summary.mean_gross_total_pnl = calc_mean(gross_pnls);

  summary.mean_turnover = calc_mean(turnovers);
  summary.mean_fees = calc_mean(fees);
  summary.mean_execution_cost_currency = calc_mean(execution_costs);
  summary.mean_max_drawdown = calc_mean(drawdowns);
  summary.mean_max_inventory_exposure = calc_mean(inventory_exposures);
  summary.mean_order_fill_rate = calc_mean(order_fill_rates);
  summary.mean_passive_fill_ratio = calc_mean(passive_fill_ratios);

  if (!completion_rates.empty()) {
    summary.mean_completion_rate = calc_mean(completion_rates);
  }
  if (!shortfalls.empty()) {
    summary.mean_implementation_shortfall_currency = calc_mean(shortfalls);
  }
  if (!parent_total_costs.empty()) {
    summary.mean_parent_total_cost_currency = calc_mean(parent_total_costs);
  }

  return summary;
}

std::string StrategyEvaluationReport::to_formatted_report() const {
  std::ostringstream out;
  out << std::fixed << std::setprecision(4);
  out << "========================================================\n";
  out << " Strategy Evaluation Report: " << strategy_name << "\n";
  out << " Seed: " << seed << " | Duration: " << duration_seconds << "s\n";
  out << "========================================================\n\n";

  out << "--- Portfolio Accounting ---\n";
  out << "  Final Inventory Position: " << final_portfolio.position_lots << " lots\n";
  out << "  Final Cash Balance:       " << final_portfolio.cash_currency << "\n";
  out << "  Marked Equity:            " << final_portfolio.total_equity_currency() << "\n";
  out << "  Average Entry Price:      " << final_portfolio.average_entry_price_ticks << " ticks\n";
  out << "  Gross Realized P&L:       " << final_portfolio.gross_realized_pnl_currency << "\n";
  out << "  Mark-to-Market Unrealized:" << final_portfolio.unrealized_pnl_currency << "\n";
  out << "  Total Fees Paid:          " << final_portfolio.total_fees_currency << "\n";
  out << "  Execution Cost vs Mid:    " << market_making.execution_cost_currency << "\n";
  out << "  Net Realized P&L:         " << final_portfolio.net_realized_pnl_currency() << "\n";
  out << "  Net Total P&L:            " << final_portfolio.net_total_pnl_currency() << "\n";
  out << "  Peak Equity:              " << final_portfolio.peak_equity_currency << "\n";
  out << "  Max Peak-to-Trough DD:    " << final_portfolio.max_drawdown_currency << "\n";
  out << "  Gross Turnover:           " << final_portfolio.gross_turnover_currency << "\n\n";

  out << "--- Market Making & Execution Profile ---\n";
  out << "  Total Trades Executed:    " << market_making.total_trades << "\n";
  out << "  Gross Total P&L:          " << market_making.gross_total_pnl << "\n";
  out << "  Passive Fills:            " << market_making.passive_fills << "\n";
  out << "  Aggressive Fills:         " << market_making.aggressive_fills << "\n";
  out << "  Passive Fill Ratio:       " << market_making.passive_fill_ratio * 100.0 << " %\n";
  out << "  Order Fill Rate:          " << market_making.order_fill_rate * 100.0 << " %\n";
  out << "  Max Inventory Exposure:   " << market_making.max_inventory_exposure << " lots\n\n";
  out << "  Remaining Open Orders:    " << final_open_orders.size() << "\n\n";

  if (parent_order.has_value()) {
    out << "--- Parent Order Execution (Benchmark) ---\n";
    out << "  Target Quantity:          " << parent_order->target_quantity << " lots\n";
    out << "  Executed Quantity:        " << parent_order->executed_quantity << " lots\n";
    out << "  Completion Rate:          " << parent_order->completion_rate * 100.0 << " %\n";
    out << "  Arrival Midprice:         " << parent_order->arrival_midprice_ticks << " ticks\n";
    out << "  Execution VWAP:           " << parent_order->execution_vwap_ticks << " ticks\n";
    out << "  Implementation Shortfall: " << parent_order->implementation_shortfall_ticks << " ticks ("
        << parent_order->implementation_shortfall_currency << " currency)\n";
    out << "  Total Execution Cost:     " << parent_order->total_cost_currency << "\n\n";
  }

  return out.str();
}

std::string StrategyEvaluationReport::to_json() const {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6);
  out << "{\n";
  out << "  \"strategy_name\": \"" << strategy_name << "\",\n";
  out << "  \"seed\": " << seed << ",\n";
  out << "  \"duration_seconds\": " << duration_seconds << ",\n";
  out << "  \"portfolio\": {\n";
  out << "    \"position_lots\": " << final_portfolio.position_lots << ",\n";
  out << "    \"cash\": " << final_portfolio.cash_currency << ",\n";
  out << "    \"marked_equity\": " << final_portfolio.total_equity_currency() << ",\n";
  out << "    \"average_entry_price_ticks\": " << final_portfolio.average_entry_price_ticks << ",\n";
  out << "    \"gross_realized_pnl\": " << final_portfolio.gross_realized_pnl_currency << ",\n";
  out << "    \"unrealized_pnl\": " << final_portfolio.unrealized_pnl_currency << ",\n";
  out << "    \"total_fees\": " << final_portfolio.total_fees_currency << ",\n";
  out << "    \"net_total_pnl\": " << final_portfolio.net_total_pnl_currency() << ",\n";
  out << "    \"gross_turnover\": " << final_portfolio.gross_turnover_currency << ",\n";
  out << "    \"max_drawdown\": " << final_portfolio.max_drawdown_currency << "\n";
  out << "  },\n";
  out << "  \"market_making\": {\n";
  out << "    \"gross_total_pnl\": " << market_making.gross_total_pnl << ",\n";
  out << "    \"total_trades\": " << market_making.total_trades << ",\n";
  out << "    \"passive_fills\": " << market_making.passive_fills << ",\n";
  out << "    \"aggressive_fills\": " << market_making.aggressive_fills << ",\n";
  out << "    \"passive_fill_ratio\": " << market_making.passive_fill_ratio << ",\n";
  out << "    \"order_fill_rate\": " << market_making.order_fill_rate << ",\n";
  out << "    \"execution_cost_currency\": " << market_making.execution_cost_currency << ",\n";
  out << "    \"max_inventory_exposure\": " << market_making.max_inventory_exposure << ",\n";
  out << "    \"open_orders\": [";
  for (std::size_t index = 0; index < final_open_orders.size(); ++index) {
    const auto& order = final_open_orders[index];
    if (index > 0) out << ",";
    out << "\n      {\"client_order_id\": " << order.client_order_id
        << ", \"side\": \"" << (order.side == core::Side::Buy ? "buy" : "sell")
        << "\", \"price_ticks\": " << order.price_ticks
        << ", \"remaining_quantity\": " << order.remaining_quantity << "}";
  }
  if (!final_open_orders.empty()) out << "\n    ";
  out << "]\n";
  out << "  }";

  if (parent_order.has_value()) {
    out << ",\n  \"parent_order\": {\n";
    out << "    \"target_quantity\": " << parent_order->target_quantity << ",\n";
    out << "    \"executed_quantity\": " << parent_order->executed_quantity << ",\n";
    out << "    \"completion_rate\": " << parent_order->completion_rate << ",\n";
    out << "    \"arrival_midprice_ticks\": " << parent_order->arrival_midprice_ticks << ",\n";
    out << "    \"execution_vwap_ticks\": " << parent_order->execution_vwap_ticks << ",\n";
    out << "    \"implementation_shortfall_ticks\": " << parent_order->implementation_shortfall_ticks << ",\n";
    out << "    \"implementation_shortfall_currency\": " << parent_order->implementation_shortfall_currency << ",\n";
    out << "    \"total_cost_currency\": " << parent_order->total_cost_currency << "\n";
    out << "  }\n";
  } else {
    out << "\n";
  }
  out << "}\n";
  return out.str();
}

std::string MultiRunEvaluationSummary::to_formatted_report() const {
  std::ostringstream out;
  out << std::fixed << std::setprecision(4);
  out << "========================================================\n";
  out << " Multi-Run Strategy Evaluation Summary: " << strategy_name << "\n";
  out << " Number of Independent Runs: " << num_runs << "\n";
  out << "========================================================\n\n";

  out << "  Mean Net Total P&L:       " << mean_net_total_pnl << " (Std: " << std_net_total_pnl
      << ", SE: " << se_net_total_pnl << ")\n";
  out << "  Mean Gross Total P&L:     " << mean_gross_total_pnl << "\n";
  out << "  Mean Turnover:            " << mean_turnover << "\n";
  out << "  Mean Fees Paid:           " << mean_fees << "\n";
  out << "  Mean Execution Cost:      " << mean_execution_cost_currency << "\n";
  out << "  Mean Max Drawdown:        " << mean_max_drawdown << "\n";
  out << "  Mean Max Inventory:       " << mean_max_inventory_exposure << " lots\n";
  out << "  Mean Order Fill Rate:     " << mean_order_fill_rate * 100.0 << " %\n";
  out << "  Mean Passive Fill Ratio:  " << mean_passive_fill_ratio * 100.0 << " %\n";

  if (parent_order_evaluation) {
    out << "  Mean Completion Rate:     " << mean_completion_rate * 100.0 << " %\n";
    out << "  Mean Implementation Shortfall: " << mean_implementation_shortfall_currency << "\n";
    out << "  Mean Parent Total Cost:   " << mean_parent_total_cost_currency << "\n";
  }
  out << "\n";
  return out.str();
}

std::string MultiRunEvaluationSummary::to_json() const {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6);
  out << "{\n";
  out << "  \"strategy_name\": \"" << strategy_name << "\",\n";
  out << "  \"num_runs\": " << num_runs << ",\n";
  out << "  \"parent_order_evaluation\": "
      << (parent_order_evaluation ? "true" : "false") << ",\n";
  out << "  \"mean_net_total_pnl\": " << mean_net_total_pnl << ",\n";
  out << "  \"std_net_total_pnl\": " << std_net_total_pnl << ",\n";
  out << "  \"se_net_total_pnl\": " << se_net_total_pnl << ",\n";
  out << "  \"mean_gross_total_pnl\": " << mean_gross_total_pnl << ",\n";
  out << "  \"mean_turnover\": " << mean_turnover << ",\n";
  out << "  \"mean_fees\": " << mean_fees << ",\n";
  out << "  \"mean_execution_cost_currency\": " << mean_execution_cost_currency << ",\n";
  out << "  \"mean_max_drawdown\": " << mean_max_drawdown << ",\n";
  out << "  \"mean_max_inventory_exposure\": " << mean_max_inventory_exposure << ",\n";
  out << "  \"mean_order_fill_rate\": " << mean_order_fill_rate << ",\n";
  out << "  \"mean_passive_fill_ratio\": " << mean_passive_fill_ratio << ",\n";
  out << "  \"mean_completion_rate\": " << mean_completion_rate << ",\n";
  out << "  \"mean_implementation_shortfall_currency\": " << mean_implementation_shortfall_currency << ",\n";
  out << "  \"mean_parent_total_cost_currency\": " << mean_parent_total_cost_currency << "\n";
  out << "}\n";
  return out.str();
}

}  // namespace lob::strategy
