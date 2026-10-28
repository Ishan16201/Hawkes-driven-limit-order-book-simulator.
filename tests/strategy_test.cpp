#include "lob/core/types.hpp"
#include "lob/exchange/matching_engine.hpp"
#include "lob/stochastic/hawkes_market_adapter.hpp"
#include "lob/strategy/baseline_strategies.hpp"
#include "lob/strategy/order_broker.hpp"
#include "lob/strategy/strategy.hpp"
#include "lob/strategy/strategy_evaluator.hpp"
#include "lob/strategy/types.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace lob;
using namespace lob::strategy;

namespace {

template <typename T>
concept ExposesFutureEvents = requires(const T& value) { value.future_events(); };

template <typename T>
concept ExposesRandomGenerator = requires(const T& value) { value.random_generator(); };

static_assert(!ExposesFutureEvents<StrategyContext>);
static_assert(!ExposesRandomGenerator<StrategyContext>);

stochastic::SyntheticMarketConfig make_test_market_config(const std::uint64_t seed = 42) {
  stochastic::SyntheticMarketConfig config;
  config.seed = seed;
  config.horizon_seconds = 2.0;

  // 6D Hawkes parameters (well-behaved stationary)
  config.hawkes.baseline_events_per_second = {5.0, 5.0, 2.0, 2.0, 1.0, 1.0};
  config.hawkes.alpha_branching = {
      {0.05, 0.01, 0.02, 0.01, 0.01, 0.01},
      {0.01, 0.05, 0.01, 0.02, 0.01, 0.01},
      {0.02, 0.01, 0.05, 0.01, 0.01, 0.01},
      {0.01, 0.02, 0.01, 0.05, 0.01, 0.01},
      {0.01, 0.01, 0.01, 0.01, 0.02, 0.01},
      {0.01, 0.01, 0.01, 0.01, 0.01, 0.02},
  };
  config.hawkes.beta_per_second = {
      {20.0, 20.0, 20.0, 20.0, 20.0, 20.0},
      {20.0, 20.0, 20.0, 20.0, 20.0, 20.0},
      {20.0, 20.0, 20.0, 20.0, 20.0, 20.0},
      {20.0, 20.0, 20.0, 20.0, 20.0, 20.0},
      {20.0, 20.0, 20.0, 20.0, 20.0, 20.0},
      {20.0, 20.0, 20.0, 20.0, 20.0, 20.0},
  };

  config.initial_book.best_bid_ticks = 99;
  config.initial_book.best_ask_ticks = 101;
  config.initial_book.levels_per_side = 5;
  config.initial_book.orders_per_level = 2;
  config.initial_book.tick_spacing = 1;
  config.initial_book.order_size = 20;

  return config;
}

}  // namespace

TEST_CASE("Event-driven Portfolio Accounting: Hand-calculated reconciliation", "[strategy][accounting]") {
  OrderBroker::Config broker_config;
  broker_config.currency_per_tick = 1.0;
  broker_config.fees.maker_fee_per_lot = 0.05;
  broker_config.fees.taker_fee_per_lot = 0.10;
  broker_config.fees.commission_per_order = 1.00;
  broker_config.risk.max_order_lots = 100;
  broker_config.risk.max_position_lots = 500;
  broker_config.risk.allow_short_selling = true;

  OrderBroker broker(broker_config);
  exchange::MatchingEngine engine;

  // Set up resting orders in engine:
  // Bid 50 @ 85, Sell 10 @ 90, Sell 10 @ 92, Sell 50 @ 100
  // Note: 85 < 90, so orders rest peacefully without crossing
  static_cast<void>(engine.submit_limit(1, 0, core::Side::Buy, 85, 50));
  static_cast<void>(engine.submit_limit(2, 0, core::Side::Sell, 90, 10));
  static_cast<void>(engine.submit_limit(3, 0, core::Side::Sell, 92, 10));
  static_cast<void>(engine.submit_limit(4, 0, core::Side::Sell, 100, 50));

  NoTradeStrategy dummy_strategy;
  REQUIRE(engine.book().best_bid().has_value());
  REQUIRE(engine.book().best_ask().has_value());

  class MockContext final : public StrategyContext {
   public:
    explicit MockContext(const OrderBroker& b) : broker_(b) {}
    core::TimestampNs current_time_ns() const noexcept override { return 100; }
    std::optional<core::PriceTicks> best_bid() const noexcept override { return 85; }
    std::optional<core::PriceTicks> best_ask() const noexcept override { return 90; }
    std::optional<double> midprice() const noexcept override { return 87.5; }
    const StrategyBookSnapshot& current_book() const noexcept override { return snapshot_; }
    const PortfolioState& portfolio() const noexcept override { return broker_.portfolio(); }
    const std::vector<OpenStrategyOrder>& open_orders() const noexcept override {
      return broker_.open_orders();
    }
   private:
    const OrderBroker& broker_;
    StrategyBookSnapshot snapshot_{};
  };

  MockContext ctx(broker);

  // Step 1: Buy 10 lots via Market Order at price 90
  // Fee = 10 * 0.10 (taker) + 1.00 = 2.00
  // Cash outflow = -(10 * 90 + 2.00) = -902.00
  broker.set_current_time(100);
  broker.submit_market(101, core::Side::Buy, 10);
  broker.process_pending_commands(engine, 100, dummy_strategy, ctx);

  const auto& p1 = broker.portfolio();
  CHECK(p1.position_lots == 10);
  CHECK_THAT(p1.cash_currency, Catch::Matchers::WithinAbs(-902.0, 1e-6));
  CHECK_THAT(p1.average_entry_price_ticks, Catch::Matchers::WithinAbs(90.0, 1e-6));
  CHECK_THAT(p1.total_fees_currency, Catch::Matchers::WithinAbs(2.0, 1e-6));
  CHECK_THAT(p1.gross_realized_pnl_currency, Catch::Matchers::WithinAbs(0.0, 1e-6));
  CHECK_THAT(p1.gross_turnover_currency, Catch::Matchers::WithinAbs(900.0, 1e-6));

  // Step 2: Buy 10 more lots via Market Order at price 92
  // Fee = 10 * 0.10 + 1.00 = 2.00
  // Cash outflow = -(10 * 92 + 2.00) = -922.00
  // Cumulative cash = -902 - 922 = -1824.00
  // New position = 20 lots.
  // Average entry price = (10 * 90 + 10 * 92) / 20 = 91.0
  broker.set_current_time(200);
  broker.submit_market(102, core::Side::Buy, 10);
  broker.process_pending_commands(engine, 200, dummy_strategy, ctx);

  const auto& p2 = broker.portfolio();
  CHECK(p2.position_lots == 20);
  CHECK_THAT(p2.cash_currency, Catch::Matchers::WithinAbs(-1824.0, 1e-6));
  CHECK_THAT(p2.average_entry_price_ticks, Catch::Matchers::WithinAbs(91.0, 1e-6));
  CHECK_THAT(p2.total_fees_currency, Catch::Matchers::WithinAbs(4.0, 1e-6));
  CHECK_THAT(p2.gross_realized_pnl_currency, Catch::Matchers::WithinAbs(0.0, 1e-6));
  CHECK_THAT(p2.gross_turnover_currency, Catch::Matchers::WithinAbs(1820.0, 1e-6));

  // Now asks at 90 and 92 are exhausted. Lowest ask in engine is 100.
  // Add a resting limit buy order in the market at 95 (95 < 100, so it rests):
  static_cast<void>(engine.submit_limit(5, 250, core::Side::Buy, 95, 50));

  // Step 3: Sell 15 lots at price 95 (matching against resting Buy 50 @ 95)
  // Fee = 15 * 0.10 + 1.00 = 2.50
  // Gross sale value = 15 * 95 = 1425.00
  // Cash flow = +1425.00 - 2.50 = +1422.50
  // Cumulative cash = -1824.00 + 1422.50 = -401.50
  // Position reduced from 20 to 5 lots.
  // Realized P&L on 15 lots = 15 * (95 - 91) = +60.00
  // Average entry price remains 91.0 for the remaining 5 lots.
  broker.set_current_time(300);
  broker.submit_market(103, core::Side::Sell, 15);
  broker.process_pending_commands(engine, 300, dummy_strategy, ctx);

  const auto& p3 = broker.portfolio();
  CHECK(p3.position_lots == 5);
  CHECK_THAT(p3.cash_currency, Catch::Matchers::WithinAbs(-401.50, 1e-6));
  CHECK_THAT(p3.average_entry_price_ticks, Catch::Matchers::WithinAbs(91.0, 1e-6));
  CHECK_THAT(p3.gross_realized_pnl_currency, Catch::Matchers::WithinAbs(60.0, 1e-6));
  CHECK_THAT(p3.total_fees_currency, Catch::Matchers::WithinAbs(6.50, 1e-6));
  CHECK_THAT(p3.net_realized_pnl_currency(), Catch::Matchers::WithinAbs(53.50, 1e-6));

  // Step 4: Mark-to-market at midprice = 93.0
  // Unrealized P&L on 5 lots = 5 * (93.0 - 91.0) = +10.00
  // Net Total P&L = Gross Realized (60.0) + Unrealized (10.0) - Total Fees (6.50) = 63.50
  // Reconcile with Equity = Cash (-401.50) + Position * Midprice (5 * 93.0 = 465.0) = 63.50
  broker.mark_to_market(93.0);
  CHECK_THAT(broker.portfolio().unrealized_pnl_currency, Catch::Matchers::WithinAbs(10.0, 1e-6));
  CHECK_THAT(broker.portfolio().net_total_pnl_currency(), Catch::Matchers::WithinAbs(63.50, 1e-6));
  CHECK_THAT(broker.portfolio().cash_currency + 5.0 * 93.0,
             Catch::Matchers::WithinAbs(broker.portfolio().net_total_pnl_currency(), 1e-6));
  CHECK_THAT(broker.portfolio().total_equity_currency(),
             Catch::Matchers::WithinAbs(broker.portfolio().net_total_pnl_currency(), 1e-6));
}

TEST_CASE("Position Flip and Short Position Accounting", "[strategy][accounting]") {
  OrderBroker::Config broker_config;
  broker_config.currency_per_tick = 1.0;
  broker_config.fees = {};  // Zero fees for clear accounting verification
  broker_config.risk.allow_short_selling = true;
  broker_config.risk.max_position_lots = 1000;

  OrderBroker broker(broker_config);
  exchange::MatchingEngine engine;
  NoTradeStrategy dummy;

  class DummyContext final : public StrategyContext {
   public:
    explicit DummyContext(const OrderBroker& b) : b_(b) {}
    core::TimestampNs current_time_ns() const noexcept override { return 0; }
    std::optional<core::PriceTicks> best_bid() const noexcept override { return 90; }
    std::optional<core::PriceTicks> best_ask() const noexcept override { return 100; }
    std::optional<double> midprice() const noexcept override { return 95.0; }
    const StrategyBookSnapshot& current_book() const noexcept override { return s_; }
    const PortfolioState& portfolio() const noexcept override { return b_.portfolio(); }
    const std::vector<OpenStrategyOrder>& open_orders() const noexcept override { return b_.open_orders(); }
   private:
    const OrderBroker& b_;
    StrategyBookSnapshot s_{};
  } ctx(broker);

  // Initial orders in engine:
  // Bid: 100 @ 90, Asks: 10 @ 100, 100 @ 120
  static_cast<void>(engine.submit_limit(1, 0, core::Side::Buy, 90, 100));
  static_cast<void>(engine.submit_limit(2, 0, core::Side::Sell, 100, 10));
  static_cast<void>(engine.submit_limit(3, 0, core::Side::Sell, 120, 100));

  // Buy 10 @ 100 -> pos = +10, entry = 100, cash = -1000
  broker.set_current_time(10);
  broker.submit_market(10, core::Side::Buy, 10);
  broker.process_pending_commands(engine, 10, dummy, ctx);
  CHECK(broker.portfolio().position_lots == 10);
  CHECK_THAT(broker.portfolio().average_entry_price_ticks, Catch::Matchers::WithinAbs(100.0, 1e-6));

  // Now ask @ 100 is filled. Lowest ask in book is 120.
  // Add a resting Buy 100 @ 110 (110 < 120, so it rests):
  static_cast<void>(engine.submit_limit(4, 15, core::Side::Buy, 110, 100));

  // Sell 25 @ 110 -> closes 10 long (realized = 10 * 10 = +100), flips to short 15 @ 110
  // Cash = -1000 + 25 * 110 = +1750
  broker.set_current_time(20);
  broker.submit_market(11, core::Side::Sell, 25);
  broker.process_pending_commands(engine, 20, dummy, ctx);

  CHECK(broker.portfolio().position_lots == -15);
  CHECK_THAT(broker.portfolio().cash_currency, Catch::Matchers::WithinAbs(1750.0, 1e-6));
  CHECK_THAT(broker.portfolio().gross_realized_pnl_currency, Catch::Matchers::WithinAbs(100.0, 1e-6));
  CHECK_THAT(broker.portfolio().average_entry_price_ticks, Catch::Matchers::WithinAbs(110.0, 1e-6));

  // Mark to market at mid = 105:
  // Short 15 from 110 marked at 105 -> gain of (110 - 105) * 15 = +75
  broker.mark_to_market(105.0);
  CHECK_THAT(broker.portfolio().unrealized_pnl_currency, Catch::Matchers::WithinAbs(75.0, 1e-6));
  CHECK_THAT(broker.portfolio().net_total_pnl_currency(), Catch::Matchers::WithinAbs(175.0, 1e-6));
  // Total equity = Cash (1750) + Position (-15) * Mid (105) = 1750 - 1575 = 175.0
  CHECK_THAT(broker.portfolio().cash_currency + (-15.0 * 105.0), Catch::Matchers::WithinAbs(175.0, 1e-6));
}

TEST_CASE("Pre-Trade Risk Limits Enforcement", "[strategy][risk]") {
  OrderBroker::Config config;
  config.risk.max_order_lots = 50;
  config.risk.max_position_lots = 100;
  config.risk.allow_short_selling = false;

  OrderBroker broker(config);
  exchange::MatchingEngine engine;
  NoTradeStrategy dummy;

  class Ctx final : public StrategyContext {
   public:
    explicit Ctx(const OrderBroker& b) : b_(b) {}
    core::TimestampNs current_time_ns() const noexcept override { return 0; }
    std::optional<core::PriceTicks> best_bid() const noexcept override { return std::nullopt; }
    std::optional<core::PriceTicks> best_ask() const noexcept override { return std::nullopt; }
    std::optional<double> midprice() const noexcept override { return std::nullopt; }
    const StrategyBookSnapshot& current_book() const noexcept override { return s_; }
    const PortfolioState& portfolio() const noexcept override { return b_.portfolio(); }
    const std::vector<OpenStrategyOrder>& open_orders() const noexcept override { return b_.open_orders(); }
   private:
    const OrderBroker& b_;
    StrategyBookSnapshot s_{};
  } ctx(broker);

  SECTION("Order size exceeding limit is immediately rejected") {
    broker.submit_limit(1, core::Side::Buy, 100, 60);  // max is 50
    CHECK(broker.commands_rejected() == 1);
    CHECK(broker.open_orders().empty());
    CHECK(broker.execution_history().size() == 1);
    CHECK(broker.execution_history().front().status == StrategyOrderStatus::Rejected);
    CHECK(broker.execution_history().front().rejection ==
          StrategyRejectionReason::OrderSizeLimitExceeded);
  }

  SECTION("Short selling is rejected when forbidden") {
    broker.submit_market(2, core::Side::Sell, 10);  // position is 0, sell makes it -10
    CHECK(broker.commands_rejected() == 1);
    CHECK(broker.execution_history().front().rejection ==
          StrategyRejectionReason::ShortSellingNotAllowed);
  }

  SECTION("Position limit breach is rejected") {
    // Fill 50 lots long
    static_cast<void>(engine.submit_limit(90, 0, core::Side::Sell, 100, 50));
    broker.submit_market(3, core::Side::Buy, 50);
    broker.process_pending_commands(engine, 0, dummy, ctx);
    CHECK(broker.portfolio().position_lots == 50);

    // Try to buy 60 lots (exceeds max 100)
    broker.submit_limit(4, core::Side::Buy, 100, 60);  // would exceed order size & position limit
    CHECK(broker.commands_rejected() > 0);
  }
}

TEST_CASE("Lookahead Audit: Strategy Context contains NO future information", "[strategy][audit]") {
  const auto market_config = make_test_market_config(12345);

  class LookaheadAuditorStrategy final : public Strategy {
   public:
    void on_start(const StrategyContext& ctx, StrategyActionEmitter&) override {
      last_time_ = ctx.current_time_ns();
      CHECK(ctx.current_time_ns() == 0);
      CHECK(ctx.best_bid().has_value());
      CHECK(ctx.best_ask().has_value());
      CHECK(*ctx.best_bid() < *ctx.best_ask());
    }

    void on_quote(const StrategyContext& ctx, StrategyActionEmitter&) override {
      // Invariant: current simulated time must be non-decreasing
      CHECK(ctx.current_time_ns() >= last_time_);
      last_time_ = ctx.current_time_ns();

      // Invariant: book snapshot timestamp must match or precede current_time_ns()
      const auto& snap = ctx.current_book();
      CHECK(snap.timestamp_ns <= ctx.current_time_ns());

      if (ctx.best_bid() && ctx.best_ask()) {
        CHECK(*ctx.best_bid() < *ctx.best_ask());
      }
      ++quote_updates_;
    }

    void on_trade(const StrategyContext& ctx, const StrategyMarketTrade& trade, StrategyActionEmitter&) override {
      CHECK(ctx.current_time_ns() >= last_time_);
      CHECK(trade.timestamp_ns <= ctx.current_time_ns());
      last_time_ = ctx.current_time_ns();
      ++trade_updates_;
    }

    void on_execution_report(const StrategyContext&, const StrategyExecutionReport& rep, StrategyActionEmitter&) override {
      CHECK(rep.timestamp_ns >= last_time_);
    }

    void on_timer(const StrategyContext& ctx, StrategyActionEmitter&) override {
      CHECK(ctx.current_time_ns() >= last_time_);
      last_time_ = ctx.current_time_ns();
      ++timer_ticks_;
    }

    void on_stop(const StrategyContext& ctx, StrategyActionEmitter&) override {
      CHECK(ctx.current_time_ns() >= last_time_);
      stopped_ = true;
    }

    [[nodiscard]] std::string name() const override { return "LookaheadAuditor"; }

    core::TimestampNs last_time_{0};
    std::size_t quote_updates_{0};
    std::size_t trade_updates_{0};
    std::size_t timer_ticks_{0};
    bool stopped_{false};
  };

  LookaheadAuditorStrategy auditor;
  StrategyEvaluatorConfig eval_config;
  eval_config.timer_interval_ns = 500'000'000;  // 0.5s timer ticks

  const auto report = StrategyEvaluator::run_single(auditor, market_config, eval_config);

  CHECK(auditor.stopped_);
  CHECK(auditor.quote_updates_ > 0);
  CHECK(auditor.timer_ticks_ >= 3);
  CHECK(report.market_making.total_trades == 0);
}

TEST_CASE("Baseline Strategies: No-Trade Benchmark", "[strategy][baseline]") {
  const auto market_config = make_test_market_config(999);
  NoTradeStrategy strategy;

  const auto report = StrategyEvaluator::run_single(strategy, market_config);

  CHECK(report.final_portfolio.position_lots == 0);
  CHECK(report.final_portfolio.cash_currency == 0.0);
  CHECK(report.final_portfolio.gross_turnover_currency == 0.0);
  CHECK(report.final_portfolio.total_fees_currency == 0.0);
  CHECK(report.final_portfolio.net_total_pnl_currency() == 0.0);
  CHECK(report.market_making.total_trades == 0);
}

TEST_CASE("Baseline Strategies: Market Order Execution Policy", "[strategy][baseline]") {
  const auto market_config = make_test_market_config(101);

  MarketOrderExecutionStrategy::Config strat_cfg;
  strat_cfg.side = core::Side::Buy;
  strat_cfg.target_quantity = 15;
  MarketOrderExecutionStrategy strategy(strat_cfg);

  StrategyEvaluatorConfig eval_cfg;
  eval_cfg.evaluate_parent_order = true;
  eval_cfg.parent_order_target = 15;
  eval_cfg.parent_order_side = core::Side::Buy;
  eval_cfg.broker_config.currency_per_tick = 1.0;
  eval_cfg.broker_config.fees.taker_fee_per_lot = 0.02;

  const auto report = StrategyEvaluator::run_single(strategy, market_config, eval_cfg);

  CHECK(report.final_portfolio.position_lots == 15);
  CHECK(report.parent_order.has_value());
  CHECK(report.parent_order->target_quantity == 15);
  CHECK(report.parent_order->executed_quantity == 15);
  CHECK_THAT(report.parent_order->completion_rate, Catch::Matchers::WithinAbs(1.0, 1e-6));
  CHECK(report.parent_order->execution_vwap_ticks > 0.0);
  CHECK(report.parent_order->total_cost_currency > 0.0);
  CHECK(report.market_making.aggressive_fills > 0);
  CHECK(report.market_making.passive_fills == 0);
  CHECK(report.market_making.execution_cost_currency > 0.0);
}

TEST_CASE("Baseline Strategies: Passive Limit Order Execution Policy", "[strategy][baseline]") {
  const auto market_config = make_test_market_config(777);

  PassiveLimitExecutionStrategy::Config strat_cfg;
  strat_cfg.side = core::Side::Buy;
  strat_cfg.target_quantity = 10;
  strat_cfg.price_offset_ticks = 0;  // Post at best bid
  PassiveLimitExecutionStrategy strategy(strat_cfg);

  StrategyEvaluatorConfig eval_cfg;
  eval_cfg.broker_config.fees.maker_fee_per_lot = -0.01;  // Liquidity rebate!
  eval_cfg.broker_config.fees.taker_fee_per_lot = 0.05;

  const auto report = StrategyEvaluator::run_single(strategy, market_config, eval_cfg);

  // If filled, orders should be passive maker fills receiving rebate
  if (report.market_making.total_trades > 0) {
    CHECK(report.market_making.passive_fills > 0);
    CHECK(report.market_making.aggressive_fills == 0);
    CHECK_THAT(report.market_making.passive_fill_ratio, Catch::Matchers::WithinAbs(1.0, 1e-6));
    // Verify maker rebate reduced fees (maker_fee is negative)
    CHECK(report.final_portfolio.total_fees_currency < 0.0);
  }
}

TEST_CASE("Baseline Strategies: TWAP Parent Order Execution", "[strategy][baseline]") {
  const auto market_config = make_test_market_config(888);

  TWAPExecutionStrategy::Config strat_cfg;
  strat_cfg.side = core::Side::Buy;
  strat_cfg.parent_order_quantity = 20;
  strat_cfg.num_slices = 4;  // 5 lots per slice
  strat_cfg.duration_ns = 2'000'000'000;
  TWAPExecutionStrategy strategy(strat_cfg);

  StrategyEvaluatorConfig eval_cfg;
  eval_cfg.timer_interval_ns = 500'000'000;  // 0.5s ticks
  eval_cfg.evaluate_parent_order = true;
  eval_cfg.parent_order_target = 20;
  eval_cfg.parent_order_side = core::Side::Buy;

  const auto report = StrategyEvaluator::run_single(strategy, market_config, eval_cfg);

  CHECK(report.parent_order.has_value());
  CHECK(report.parent_order->target_quantity == 20);
  CHECK(report.parent_order->executed_quantity == 20);
  CHECK_THAT(report.parent_order->completion_rate, Catch::Matchers::WithinAbs(1.0, 1e-6));
  CHECK(report.parent_order->execution_vwap_ticks >= 100.0);
  CHECK(report.final_portfolio.position_lots == 20);
}

TEST_CASE("Multi-Run Evaluation across Seeds with Uncertainty Reporting", "[strategy][multirun]") {
  const auto market_config = make_test_market_config(10);
  const std::vector<std::uint64_t> seeds = {101, 102, 103, 104, 105};

  StrategyEvaluatorConfig eval_cfg;
  eval_cfg.timer_interval_ns = 500'000'000;
  eval_cfg.evaluate_parent_order = true;
  eval_cfg.parent_order_target = 10;
  eval_cfg.parent_order_side = core::Side::Buy;

  auto factory = []() -> std::unique_ptr<Strategy> {
    MarketOrderExecutionStrategy::Config cfg;
    cfg.side = core::Side::Buy;
    cfg.target_quantity = 10;
    return std::make_unique<MarketOrderExecutionStrategy>(cfg);
  };

  const auto summary = StrategyEvaluator::run_multiple(factory, market_config, seeds, eval_cfg);

  CHECK(summary.num_runs == 5);
  CHECK(summary.individual_runs.size() == 5);
  CHECK(summary.strategy_name == "MarketOrderExecution");
  CHECK_THAT(summary.mean_completion_rate, Catch::Matchers::WithinAbs(1.0, 1e-6));
  CHECK_THAT(summary.mean_order_fill_rate, Catch::Matchers::WithinAbs(1.0, 1e-6));
  CHECK(summary.mean_turnover > 0.0);
  CHECK(summary.mean_execution_cost_currency > 0.0);

  // Formatted report & JSON output validation
  const std::string text_report = summary.to_formatted_report();
  CHECK(text_report.find("Multi-Run Strategy Evaluation Summary") != std::string::npos);
  CHECK(text_report.find("Number of Independent Runs: 5") != std::string::npos);

  const std::string json_report = summary.to_json();
  CHECK(json_report.find("\"strategy_name\": \"MarketOrderExecution\"") != std::string::npos);
  CHECK(json_report.find("\"num_runs\": 5") != std::string::npos);
  CHECK(json_report.find("\"mean_order_fill_rate\"") != std::string::npos);
}

TEST_CASE("Partial fills, cancellation, fees, and marks reconcile", "[strategy][accounting]") {
  OrderBroker::Config config;
  config.fees.maker_fee_per_lot = 0.05;
  config.fees.commission_per_order = 1.0;
  config.risk.max_order_lots = 100;
  config.risk.max_position_lots = 100;

  OrderBroker broker(config);
  exchange::MatchingEngine engine;
  NoTradeStrategy strategy;
  static_cast<void>(engine.submit_limit(1, 0, core::Side::Sell, 100, 10));

  class Context final : public StrategyContext {
   public:
    explicit Context(const OrderBroker& broker) : broker_(broker) {}
    core::TimestampNs current_time_ns() const noexcept override { return time_ns; }
    std::optional<core::PriceTicks> best_bid() const noexcept override { return 99; }
    std::optional<core::PriceTicks> best_ask() const noexcept override { return 100; }
    std::optional<double> midprice() const noexcept override { return 99.5; }
    const StrategyBookSnapshot& current_book() const noexcept override { return book_; }
    const PortfolioState& portfolio() const noexcept override { return broker_.portfolio(); }
    const std::vector<OpenStrategyOrder>& open_orders() const noexcept override {
      return broker_.open_orders();
    }
    core::TimestampNs time_ns{0};
   private:
    const OrderBroker& broker_;
    StrategyBookSnapshot book_{};
  } ctx(broker);

  broker.set_current_time(0);
  broker.submit_limit(10, core::Side::Buy, 99, 10);
  broker.process_pending_commands(engine, 0, strategy, ctx);
  REQUIRE(broker.open_orders().size() == 1);

  const auto first_partial = engine.submit_market(2, 1, core::Side::Sell, 2);
  REQUIRE(first_partial.accepted);
  REQUIRE(first_partial.trades.size() == 1);
  broker.process_trade(first_partial.trades.front(), strategy, ctx);
  const auto second_partial = engine.submit_market(3, 2, core::Side::Sell, 2);
  REQUIRE(second_partial.accepted);
  REQUIRE(second_partial.trades.size() == 1);
  broker.process_trade(second_partial.trades.front(), strategy, ctx);
  ctx.time_ns = 2;
  broker.process_pending_reports(2, strategy, ctx);

  CHECK(broker.portfolio().position_lots == 4);
  CHECK_THAT(broker.portfolio().cash_currency, Catch::Matchers::WithinAbs(-397.2, 1e-9));
  CHECK_THAT(broker.portfolio().total_fees_currency, Catch::Matchers::WithinAbs(1.2, 1e-9));
  CHECK(broker.open_orders().front().remaining_quantity == 6);

  broker.set_current_time(3);
  broker.cancel(10);
  broker.process_pending_commands(engine, 3, strategy, ctx);
  ctx.time_ns = 3;
  broker.process_pending_reports(3, strategy, ctx);

  CHECK(broker.open_orders().empty());
  CHECK(broker.total_trades() == 2);
  CHECK(broker.execution_history().size() == 3);
  CHECK(broker.execution_history().back().status == StrategyOrderStatus::Cancelled);
  CHECK(broker.execution_history().back().filled_quantity == 0);

  broker.set_current_time(4);
  broker.submit_limit(11, core::Side::Buy, 98, 2);
  broker.process_pending_commands(engine, 4, strategy, ctx);
  REQUIRE(broker.open_orders().size() == 1);
  const auto external_cancel = engine.cancel(11, 5);
  REQUIRE(external_cancel.accepted);
  broker.process_external_cancellation(11, 5);
  ctx.time_ns = 5;
  broker.process_pending_reports(5, strategy, ctx);
  CHECK(broker.open_orders().empty());
  CHECK(broker.total_trades() == 2);

  broker.mark_to_market(100.0);
  CHECK_THAT(broker.portfolio().unrealized_pnl_currency, Catch::Matchers::WithinAbs(4.0, 1e-9));
  CHECK_THAT(broker.portfolio().net_total_pnl_currency(), Catch::Matchers::WithinAbs(2.8, 1e-9));
  CHECK_THAT(broker.portfolio().cash_currency + 4.0 * 100.0,
             Catch::Matchers::WithinAbs(broker.portfolio().net_total_pnl_currency(), 1e-9));
  CHECK_THAT(broker.portfolio().total_equity_currency(),
             Catch::Matchers::WithinAbs(broker.portfolio().net_total_pnl_currency(), 1e-9));
}

TEST_CASE("Order broker enforces reserved inventory and drawdown limits", "[strategy][risk]") {
  OrderBroker::Config config;
  config.risk.max_order_lots = 10;
  config.risk.max_position_lots = 10;
  OrderBroker broker(config);
  exchange::MatchingEngine engine;
  NoTradeStrategy strategy;

  class Context final : public StrategyContext {
   public:
    explicit Context(const OrderBroker& broker) : broker_(broker) {}
    core::TimestampNs current_time_ns() const noexcept override { return 0; }
    std::optional<core::PriceTicks> best_bid() const noexcept override { return std::nullopt; }
    std::optional<core::PriceTicks> best_ask() const noexcept override { return std::nullopt; }
    std::optional<double> midprice() const noexcept override { return std::nullopt; }
    const StrategyBookSnapshot& current_book() const noexcept override { return snapshot_; }
    const PortfolioState& portfolio() const noexcept override { return broker_.portfolio(); }
    const std::vector<OpenStrategyOrder>& open_orders() const noexcept override {
      return broker_.open_orders();
    }
   private:
    const OrderBroker& broker_;
    StrategyBookSnapshot snapshot_{};
  } ctx(broker);

  broker.submit_limit(1, core::Side::Buy, 90, 8);
  broker.submit_limit(2, core::Side::Buy, 90, 8);
  CHECK(broker.commands_rejected() == 1);
  broker.process_pending_commands(engine, 0, strategy, ctx);
  REQUIRE(broker.open_orders().size() == 1);
  broker.submit_limit(3, core::Side::Buy, 90, 3);
  CHECK(broker.execution_history().back().rejection ==
        StrategyRejectionReason::PositionLimitExceeded);

  config.risk.max_drawdown_currency = 5.0;
  OrderBroker drawdown_broker(config);
  exchange::MatchingEngine drawdown_engine;
  static_cast<void>(drawdown_engine.submit_limit(20, 0, core::Side::Sell, 100, 10));
  Context drawdown_context(drawdown_broker);
  drawdown_broker.submit_market(21, core::Side::Buy, 1);
  drawdown_broker.process_pending_commands(drawdown_engine, 0, strategy, drawdown_context);
  drawdown_broker.mark_to_market(90.0);
  drawdown_broker.set_current_time(1);
  drawdown_broker.submit_market(22, core::Side::Buy, 1);
  CHECK(drawdown_broker.execution_history().back().rejection ==
        StrategyRejectionReason::RiskHaltActive);
}

TEST_CASE("Inbound reports respect latency and causal context time", "[strategy][latency][audit]") {
  OrderBroker::Config config;
  config.latency.outbound_delay_ns = 100;
  config.latency.inbound_delay_ns = 200;
  OrderBroker broker(config);
  exchange::MatchingEngine engine;
  static_cast<void>(engine.submit_limit(1, 0, core::Side::Sell, 100, 10));

  class Context final : public StrategyContext {
   public:
    explicit Context(const OrderBroker& broker) : broker_(broker) {}
    core::TimestampNs current_time_ns() const noexcept override { return time_ns; }
    std::optional<core::PriceTicks> best_bid() const noexcept override { return std::nullopt; }
    std::optional<core::PriceTicks> best_ask() const noexcept override { return 100; }
    std::optional<double> midprice() const noexcept override { return std::nullopt; }
    const StrategyBookSnapshot& current_book() const noexcept override { return snapshot_; }
    const PortfolioState& portfolio() const noexcept override { return broker_.portfolio(); }
    const std::vector<OpenStrategyOrder>& open_orders() const noexcept override {
      return broker_.open_orders();
    }
    core::TimestampNs time_ns{0};
   private:
    const OrderBroker& broker_;
    StrategyBookSnapshot snapshot_{};
  } ctx(broker);

  class Recorder final : public Strategy {
   public:
    void on_start(const StrategyContext&, StrategyActionEmitter&) override {}
    void on_quote(const StrategyContext&, StrategyActionEmitter&) override {}
    void on_trade(const StrategyContext&, const StrategyMarketTrade&, StrategyActionEmitter&) override {}
    void on_execution_report(const StrategyContext& ctx,
                             const StrategyExecutionReport& report,
                             StrategyActionEmitter&) override {
      received_at = ctx.current_time_ns();
      report_at = report.timestamp_ns;
    }
    void on_timer(const StrategyContext&, StrategyActionEmitter&) override {}
    void on_stop(const StrategyContext&, StrategyActionEmitter&) override {}
    [[nodiscard]] std::string name() const override { return "Recorder"; }
    core::TimestampNs received_at{-1};
    core::TimestampNs report_at{-1};
  } recorder;

  broker.submit_market(2, core::Side::Buy, 1);
  CHECK(broker.next_command_time() == 100);
  broker.process_pending_commands(engine, 99, recorder, ctx);
  CHECK(broker.portfolio().position_lots == 0);
  broker.process_pending_commands(engine, 100, recorder, ctx);
  CHECK(broker.portfolio().position_lots == 1);
  CHECK(broker.next_report_time() == 300);
  ctx.time_ns = 299;
  broker.process_pending_reports(299, recorder, ctx);
  CHECK(recorder.received_at == -1);
  ctx.time_ns = 300;
  broker.process_pending_reports(300, recorder, ctx);
  CHECK(recorder.received_at == 300);
  CHECK(recorder.report_at == 300);
}

TEST_CASE("TWAP dispatches slices across configured duration", "[strategy][baseline][twap]") {
  auto market_config = make_test_market_config(123);
  market_config.horizon_seconds = 1.0;
  std::fill(market_config.hawkes.baseline_events_per_second.begin(),
            market_config.hawkes.baseline_events_per_second.end(), 1.0e-12);

  TWAPExecutionStrategy::Config strategy_config;
  strategy_config.parent_order_quantity = 3;
  strategy_config.num_slices = 3;
  strategy_config.duration_ns = 1'000'000'000;
  TWAPExecutionStrategy strategy(strategy_config);

  StrategyEvaluatorConfig eval_config;
  eval_config.timer_interval_ns = 100'000'000;
  const auto report = StrategyEvaluator::run_single(strategy, market_config, eval_config);

  std::vector<core::TimestampNs> fill_times;
  for (const auto& execution : report.execution_history) {
    if (execution.filled_quantity > 0) fill_times.push_back(execution.timestamp_ns);
  }
  REQUIRE(fill_times.size() == 3);
  CHECK(fill_times[0] == 0);
  CHECK(fill_times[1] == 500'000'000);
  CHECK(fill_times[2] == 1'000'000'000);
  CHECK(report.final_open_orders.empty());
}
