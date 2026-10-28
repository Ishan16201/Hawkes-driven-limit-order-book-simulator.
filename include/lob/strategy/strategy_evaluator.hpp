#pragma once

#include "lob/stochastic/hawkes_market_adapter.hpp"
#include "lob/strategy/order_broker.hpp"
#include "lob/strategy/strategy.hpp"
#include "lob/strategy/types.hpp"

#include <memory>
#include <vector>

namespace lob::strategy {

struct StrategyEvaluatorConfig final {
  OrderBroker::Config broker_config{};
  core::DurationNs timer_interval_ns{1'000'000'000};  // 1-second default timer tick
  bool evaluate_parent_order{false};
  core::QuantityLots parent_order_target{0};
  core::Side parent_order_side{core::Side::Buy};
};

/// Multi-run aggregate evaluation summary across different seeds.
struct MultiRunEvaluationSummary final {
  std::string strategy_name;
  std::size_t num_runs{0};
  bool parent_order_evaluation{false};

  double mean_net_total_pnl{0.0};
  double std_net_total_pnl{0.0};
  double se_net_total_pnl{0.0};
  double mean_gross_total_pnl{0.0};

  double mean_turnover{0.0};
  double mean_fees{0.0};
  double mean_execution_cost_currency{0.0};
  double mean_max_drawdown{0.0};
  double mean_max_inventory_exposure{0.0};
  double mean_order_fill_rate{0.0};
  double mean_passive_fill_ratio{0.0};
  double mean_completion_rate{0.0};
  double mean_implementation_shortfall_currency{0.0};
  double mean_parent_total_cost_currency{0.0};

  std::vector<StrategyEvaluationReport> individual_runs;

  [[nodiscard]] std::string to_formatted_report() const;
  [[nodiscard]] std::string to_json() const;
};

class StrategyEvaluator final {
 public:
  /// Evaluates a single strategy run on a simulated market.
  [[nodiscard]] static StrategyEvaluationReport run_single(
      Strategy& strategy,
      const stochastic::SyntheticMarketConfig& market_config,
      const StrategyEvaluatorConfig& eval_config = {});

  /// Evaluates a strategy across multiple randomized seeds and summarizes cross-run uncertainty.
  [[nodiscard]] static MultiRunEvaluationSummary run_multiple(
      std::function<std::unique_ptr<Strategy>()> strategy_factory,
      stochastic::SyntheticMarketConfig base_market_config,
      const std::vector<std::uint64_t>& seeds,
      const StrategyEvaluatorConfig& eval_config = {});
};

}  // namespace lob::strategy
