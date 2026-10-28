#pragma once

#include "lob/strategy/strategy.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace lob::strategy {

/// 1. No-Trade Baseline Benchmark.
/// Never submits any orders; used as reference benchmark (0 turnover, 0 PnL, 0 fees).
class NoTradeStrategy final : public Strategy {
 public:
  void on_start(const StrategyContext&, StrategyActionEmitter&) override {}
  void on_quote(const StrategyContext&, StrategyActionEmitter&) override {}
  void on_trade(const StrategyContext&, const StrategyMarketTrade&, StrategyActionEmitter&) override {}
  void on_execution_report(const StrategyContext&, const StrategyExecutionReport&, StrategyActionEmitter&) override {}
  void on_timer(const StrategyContext&, StrategyActionEmitter&) override {}
  void on_stop(const StrategyContext&, StrategyActionEmitter&) override {}
  [[nodiscard]] std::string name() const override { return "NoTradeBenchmark"; }
};

/// 2. Passive Limit-Order Execution Policy.
/// Posts a resting limit order at the prevailing best quote to capture spread and rebates.
class PassiveLimitExecutionStrategy final : public Strategy {
 public:
  struct Config {
    core::Side side{core::Side::Buy};
    core::QuantityLots target_quantity{10};
    core::PriceTicks price_offset_ticks{0};  // 0 = at best quote
  };

  explicit PassiveLimitExecutionStrategy(Config config)
      : config_(config), remaining_quantity_(config.target_quantity) {}

  void on_start(const StrategyContext& ctx, StrategyActionEmitter& emitter) override {
    post_or_update_order(ctx, emitter);
  }

  void on_quote(const StrategyContext& ctx, StrategyActionEmitter& emitter) override {
    // If quote moved and we haven't filled, optionally adjust resting price
    if (active_order_id_.has_value() && remaining_quantity_ > 0) {
      post_or_update_order(ctx, emitter);
    }
  }

  void on_trade(const StrategyContext&, const StrategyMarketTrade&, StrategyActionEmitter&) override {}

  void on_execution_report(const StrategyContext& ctx,
                           const StrategyExecutionReport& report,
                           StrategyActionEmitter& emitter) override {
    if (report.client_order_id == active_order_id_.value_or(0)) {
      if (report.filled_quantity > 0) {
        remaining_quantity_ = (remaining_quantity_ > report.filled_quantity)
                                  ? (remaining_quantity_ - report.filled_quantity)
                                  : 0;
      }
      if (report.status == StrategyOrderStatus::Filled ||
          report.status == StrategyOrderStatus::Cancelled) {
        active_order_id_ = std::nullopt;
        if (remaining_quantity_ > 0) {
          post_or_update_order(ctx, emitter);
        }
      } else if (report.status == StrategyOrderStatus::Rejected ||
                 report.status == StrategyOrderStatus::Expired) {
        active_order_id_ = std::nullopt;
      }
    }
  }

  void on_timer(const StrategyContext&, StrategyActionEmitter&) override {}
  void on_stop(const StrategyContext&, StrategyActionEmitter& emitter) override {
    if (active_order_id_.has_value()) {
      emitter.cancel(*active_order_id_);
      active_order_id_ = std::nullopt;
    }
  }

  [[nodiscard]] std::string name() const override { return "PassiveLimitExecution"; }
  [[nodiscard]] core::QuantityLots remaining_quantity() const noexcept { return remaining_quantity_; }

 private:
  void post_or_update_order(const StrategyContext& ctx, StrategyActionEmitter& emitter) {
    if (remaining_quantity_ == 0) return;

    core::PriceTicks target_price = 0;
    if (config_.side == core::Side::Buy) {
      const auto bb = ctx.best_bid();
      if (!bb.has_value()) return;
      target_price = *bb + config_.price_offset_ticks;
    } else {
      const auto ba = ctx.best_ask();
      if (!ba.has_value()) return;
      target_price = *ba - config_.price_offset_ticks;
    }

    if (active_order_id_.has_value()) {
      if (last_posted_price_ == target_price) {
        return;  // Already resting at optimal price
      }
      emitter.cancel(*active_order_id_);
      active_order_id_ = std::nullopt;
    }

    const core::OrderId new_id = next_order_id_++;
    emitter.submit_limit(new_id, config_.side, target_price, remaining_quantity_);
    active_order_id_ = new_id;
    last_posted_price_ = target_price;
  }

  Config config_;
  core::QuantityLots remaining_quantity_{0};
  std::optional<core::OrderId> active_order_id_{};
  core::PriceTicks last_posted_price_{0};
  core::OrderId next_order_id_{10001};
};

/// 3. Immediate Market-Order Execution Policy.
/// Executes the target quantity immediately using aggressive market order flow.
class MarketOrderExecutionStrategy final : public Strategy {
 public:
  struct Config {
    core::Side side{core::Side::Buy};
    core::QuantityLots target_quantity{10};
  };

  explicit MarketOrderExecutionStrategy(Config config)
      : config_(config), remaining_quantity_(config.target_quantity) {}

  void on_start(const StrategyContext&, StrategyActionEmitter& emitter) override {
    if (remaining_quantity_ > 0 && !submitted_) {
      submitted_ = true;
      emitter.submit_market(order_id_, config_.side, remaining_quantity_);
    }
  }

  void on_quote(const StrategyContext&, StrategyActionEmitter&) override {}
  void on_trade(const StrategyContext&, const StrategyMarketTrade&, StrategyActionEmitter&) override {}

  void on_execution_report(const StrategyContext&,
                           const StrategyExecutionReport& report,
                           StrategyActionEmitter&) override {
    if (report.client_order_id == order_id_) {
      if (report.filled_quantity > 0) {
        remaining_quantity_ = (remaining_quantity_ > report.filled_quantity)
                                  ? (remaining_quantity_ - report.filled_quantity)
                                  : 0;
      }
    }
  }

  void on_timer(const StrategyContext&, StrategyActionEmitter&) override {}
  void on_stop(const StrategyContext&, StrategyActionEmitter&) override {}

  [[nodiscard]] std::string name() const override { return "MarketOrderExecution"; }
  [[nodiscard]] core::QuantityLots remaining_quantity() const noexcept { return remaining_quantity_; }

 private:
  Config config_;
  core::QuantityLots remaining_quantity_{0};
  core::OrderId order_id_{20001};
  bool submitted_{false};
};

/// 4. TWAP (Time-Weighted Average Price) Execution Policy.
/// Slices a fixed parent order into N_slices equal tranches across a specified duration.
class TWAPExecutionStrategy final : public Strategy {
 public:
  struct Config {
    core::Side side{core::Side::Buy};
    core::QuantityLots parent_order_quantity{100};
    std::size_t num_slices{5};
    core::DurationNs duration_ns{10'000'000'000};  // e.g. 10 seconds
  };

  explicit TWAPExecutionStrategy(Config config)
      : config_(config), remaining_parent_quantity_(config.parent_order_quantity) {
    if (config_.num_slices == 0 || config_.duration_ns < 0 ||
        (config_.num_slices > 1 && config_.duration_ns == 0)) {
      throw std::invalid_argument{"TWAP requires positive slices and a valid duration"};
    }
    slice_size_ = config_.parent_order_quantity / config_.num_slices +
                  (config_.parent_order_quantity % config_.num_slices == 0 ? 0U : 1U);
  }

  void on_start(const StrategyContext& ctx, StrategyActionEmitter& emitter) override {
    start_time_ns_ = ctx.current_time_ns();
    // Record arrival midprice
    if (ctx.midprice().has_value()) {
      arrival_midprice_ = *ctx.midprice();
    }
    // Execute slice 0
    execute_next_slice(emitter);
  }

  void on_quote(const StrategyContext& ctx, StrategyActionEmitter&) override {
    if (!arrival_midprice_.has_value() && ctx.midprice().has_value()) {
      arrival_midprice_ = *ctx.midprice();
    }
  }

  void on_trade(const StrategyContext&, const StrategyMarketTrade&, StrategyActionEmitter&) override {}

  void on_execution_report(const StrategyContext&,
                           const StrategyExecutionReport& report,
                           StrategyActionEmitter&) override {
    if (order_ids_.contains(report.client_order_id) && report.filled_quantity > 0) {
      remaining_parent_quantity_ = (remaining_parent_quantity_ > report.filled_quantity)
                                       ? (remaining_parent_quantity_ - report.filled_quantity)
                                       : 0;
    }
  }

  void on_timer(const StrategyContext& ctx, StrategyActionEmitter& emitter) override {
    while (current_slice_ < config_.num_slices &&
           submitted_quantity_ < config_.parent_order_quantity &&
           ctx.current_time_ns() >= next_slice_time_ns()) {
      execute_next_slice(emitter);
    }
  }

  void on_stop(const StrategyContext&, StrategyActionEmitter&) override {}

  [[nodiscard]] std::string name() const override { return "TWAPExecution"; }
  [[nodiscard]] core::QuantityLots remaining_parent_quantity() const noexcept {
    return remaining_parent_quantity_;
  }
  [[nodiscard]] std::optional<double> arrival_midprice() const noexcept {
    return arrival_midprice_;
  }

 private:
  void execute_next_slice(StrategyActionEmitter& emitter) {
    if (submitted_quantity_ >= config_.parent_order_quantity) return;
    const auto left_to_schedule = config_.parent_order_quantity - submitted_quantity_;
    const core::QuantityLots this_slice = std::min(slice_size_, left_to_schedule);
    const auto order_id = next_order_id_++;
    order_ids_.insert(order_id);
    emitter.submit_market(order_id, config_.side, this_slice);
    submitted_quantity_ += this_slice;
    ++current_slice_;
  }

  [[nodiscard]] core::TimestampNs next_slice_time_ns() const noexcept {
    if (config_.num_slices <= 1) return start_time_ns_;
    const auto intervals = config_.num_slices - 1;
    const auto offset = static_cast<long double>(config_.duration_ns) *
                        static_cast<long double>(current_slice_) /
                        static_cast<long double>(intervals);
    const auto scheduled_time =
        static_cast<long double>(start_time_ns_) + offset;
    if (scheduled_time >=
        static_cast<long double>(std::numeric_limits<core::TimestampNs>::max())) {
      return std::numeric_limits<core::TimestampNs>::max();
    }
    return static_cast<core::TimestampNs>(scheduled_time);
  }

  Config config_;
  core::QuantityLots remaining_parent_quantity_{0};
  core::QuantityLots slice_size_{0};
  core::QuantityLots submitted_quantity_{0};
  std::size_t current_slice_{0};
  core::TimestampNs start_time_ns_{0};
  std::optional<double> arrival_midprice_{};
  core::OrderId next_order_id_{30001};
  std::unordered_set<core::OrderId> order_ids_;
};

}  // namespace lob::strategy
