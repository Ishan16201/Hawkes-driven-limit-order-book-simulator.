#include "lob/strategy/order_broker.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace lob::strategy {
namespace {

core::TimestampNs add_delay(const core::TimestampNs timestamp_ns,
                            const core::DurationNs delay_ns) {
  if (!core::is_valid_timestamp(timestamp_ns) || delay_ns < 0) {
    throw std::invalid_argument{"strategy event time and latency must be nonnegative"};
  }
  if (delay_ns > std::numeric_limits<core::TimestampNs>::max() - timestamp_ns) {
    throw std::overflow_error{"strategy latency exceeds the timestamp range"};
  }
  return timestamp_ns + delay_ns;
}

bool valid_money(const double value) noexcept {
  return std::isfinite(value);
}

}  // namespace

OrderBroker::OrderBroker()
    : OrderBroker(Config{}) {}

OrderBroker::OrderBroker(Config config)
    : config_(config) {
  if (!std::isfinite(config_.currency_per_tick) || config_.currency_per_tick <= 0.0 ||
      !valid_money(config_.fees.maker_fee_per_lot) ||
      !valid_money(config_.fees.taker_fee_per_lot) ||
      !valid_money(config_.fees.commission_per_order) ||
      config_.fees.commission_per_order < 0.0 ||
      config_.latency.outbound_delay_ns < 0 ||
      config_.latency.inbound_delay_ns < 0 ||
      config_.risk.max_position_lots >
          static_cast<core::QuantityLots>(std::numeric_limits<std::int64_t>::max()) ||
      !std::isfinite(config_.risk.max_drawdown_currency) ||
      config_.risk.max_drawdown_currency < 0.0) {
    throw std::invalid_argument{"invalid order broker configuration"};
  }
  portfolio_.cash_currency = 0.0;
  portfolio_.position_lots = 0;
  portfolio_.peak_equity_currency = 0.0;
  portfolio_.max_drawdown_currency = 0.0;
}

void OrderBroker::set_current_time(const core::TimestampNs current_time_ns) {
  core::require_valid_timestamp(current_time_ns);
  current_time_ns_ = current_time_ns;
}

bool OrderBroker::risk_halt_active() const noexcept {
  return portfolio_.max_drawdown_currency >= config_.risk.max_drawdown_currency;
}

bool OrderBroker::would_breach_position_limit(
    const core::Side side, const core::QuantityLots quantity) const noexcept {
  long double lowest = static_cast<long double>(portfolio_.position_lots);
  long double highest = lowest;

  const auto include_order = [&lowest, &highest](const core::Side order_side,
                                                  const core::QuantityLots order_quantity) {
    const auto signed_quantity = static_cast<long double>(order_quantity);
    if (order_side == core::Side::Buy) {
      highest += signed_quantity;
    } else {
      lowest -= signed_quantity;
    }
  };

  for (const auto& order : open_orders_list_) {
    include_order(order.side, order.remaining_quantity);
  }
  for (const auto& queued : pending_commands_) {
    if (!queued.is_cancel) {
      include_order(queued.command.side, queued.command.quantity_lots);
    }
  }
  include_order(side, quantity);

  const auto limit = static_cast<long double>(config_.risk.max_position_lots);
  return lowest < -limit || highest > limit ||
         (!config_.risk.allow_short_selling && lowest < 0.0L);
}

bool OrderBroker::would_breach_short_selling(
    const core::Side side, const core::QuantityLots quantity) const noexcept {
  if (config_.risk.allow_short_selling) return false;
  long double lowest = static_cast<long double>(portfolio_.position_lots);
  for (const auto& order : open_orders_list_) {
    if (order.side == core::Side::Sell) {
      lowest -= static_cast<long double>(order.remaining_quantity);
    }
  }
  for (const auto& queued : pending_commands_) {
    if (!queued.is_cancel && queued.command.side == core::Side::Sell) {
      lowest -= static_cast<long double>(queued.command.quantity_lots);
    }
  }
  if (side == core::Side::Sell) {
    lowest -= static_cast<long double>(quantity);
  }
  return lowest < 0.0L;
}

void OrderBroker::queue_execution_report(StrategyExecutionReport report) {
  report.timestamp_ns = add_delay(report.timestamp_ns, config_.latency.inbound_delay_ns);
  execution_history_.push_back(report);
  pending_reports_.push_back(QueuedReport{
      .receipt_time_ns = report.timestamp_ns,
      .report = std::move(report),
  });
}

void OrderBroker::submit_limit(const core::OrderId client_order_id,
                              const core::Side side,
                              const core::PriceTicks price_ticks,
                              const core::QuantityLots quantity) {
  ++commands_submitted_;
  submitted_order_ids_.insert(client_order_id);

  StrategyExecutionReport rep;
  rep.client_order_id = client_order_id;
  rep.timestamp_ns = current_time_ns_;
  rep.side = side;
  rep.order_type = StrategyOrderType::Limit;

  if (risk_halt_active()) {
    ++commands_rejected_;
    rep.status = StrategyOrderStatus::Rejected;
    rep.rejection = StrategyRejectionReason::RiskHaltActive;
    queue_execution_report(std::move(rep));
    return;
  }
  if (!core::is_valid_side(side) || price_ticks <= 0 || quantity == 0 ||
      quantity > config_.risk.max_order_lots ||
      quantity > static_cast<core::QuantityLots>(std::numeric_limits<std::int64_t>::max())) {
    ++commands_rejected_;
    rep.status = StrategyOrderStatus::Rejected;
    rep.rejection = (!core::is_valid_side(side) || price_ticks <= 0)
                        ? StrategyRejectionReason::InvalidOrder
                        : StrategyRejectionReason::OrderSizeLimitExceeded;
    queue_execution_report(std::move(rep));
    return;
  }
  if (would_breach_position_limit(side, quantity)) {
    ++commands_rejected_;
    rep.status = StrategyOrderStatus::Rejected;
    rep.rejection = would_breach_short_selling(side, quantity)
                        ? StrategyRejectionReason::ShortSellingNotAllowed
                        : StrategyRejectionReason::PositionLimitExceeded;
    queue_execution_report(std::move(rep));
    return;
  }

  QueuedCommand cmd;
  cmd.execute_time_ns = add_delay(current_time_ns_, config_.latency.outbound_delay_ns);
  cmd.is_cancel = false;
  cmd.command = StrategyOrderCommand{
      .client_order_id = client_order_id,
      .timestamp_ns = current_time_ns_,
      .type = StrategyOrderType::Limit,
      .side = side,
      .price_ticks = price_ticks,
      .quantity_lots = quantity,
  };
  pending_commands_.push_back(cmd);
}

void OrderBroker::submit_market(const core::OrderId client_order_id,
                               const core::Side side,
                               const core::QuantityLots quantity) {
  ++commands_submitted_;
  submitted_order_ids_.insert(client_order_id);

  StrategyExecutionReport rep;
  rep.client_order_id = client_order_id;
  rep.timestamp_ns = current_time_ns_;
  rep.side = side;
  rep.order_type = StrategyOrderType::Market;

  if (risk_halt_active()) {
    ++commands_rejected_;
    rep.status = StrategyOrderStatus::Rejected;
    rep.rejection = StrategyRejectionReason::RiskHaltActive;
    queue_execution_report(std::move(rep));
    return;
  }
  if (!core::is_valid_side(side) || quantity == 0 ||
      quantity > config_.risk.max_order_lots ||
      quantity > static_cast<core::QuantityLots>(std::numeric_limits<std::int64_t>::max())) {
    ++commands_rejected_;
    rep.status = StrategyOrderStatus::Rejected;
    rep.rejection = !core::is_valid_side(side) ? StrategyRejectionReason::InvalidOrder
                                               : StrategyRejectionReason::OrderSizeLimitExceeded;
    queue_execution_report(std::move(rep));
    return;
  }
  if (would_breach_position_limit(side, quantity)) {
    ++commands_rejected_;
    rep.status = StrategyOrderStatus::Rejected;
    rep.rejection = would_breach_short_selling(side, quantity)
                        ? StrategyRejectionReason::ShortSellingNotAllowed
                        : StrategyRejectionReason::PositionLimitExceeded;
    queue_execution_report(std::move(rep));
    return;
  }

  QueuedCommand cmd;
  cmd.execute_time_ns = add_delay(current_time_ns_, config_.latency.outbound_delay_ns);
  cmd.is_cancel = false;
  cmd.command = StrategyOrderCommand{
      .client_order_id = client_order_id,
      .timestamp_ns = current_time_ns_,
      .type = StrategyOrderType::Market,
      .side = side,
      .price_ticks = 0,
      .quantity_lots = quantity,
  };
  pending_commands_.push_back(cmd);
}

void OrderBroker::cancel(const core::OrderId client_order_id) {
  ++commands_submitted_;
  const bool pending_submission = std::any_of(
      pending_commands_.begin(), pending_commands_.end(),
      [client_order_id](const QueuedCommand& queued) {
        return !queued.is_cancel &&
               queued.command.client_order_id == client_order_id;
      });
  if (!open_orders_map_.contains(client_order_id) && !pending_submission) {
    ++commands_rejected_;
    StrategyExecutionReport report;
    report.client_order_id = client_order_id;
    report.timestamp_ns = current_time_ns_;
    report.status = StrategyOrderStatus::Rejected;
    report.rejection = StrategyRejectionReason::UnknownOrder;
    queue_execution_report(std::move(report));
    return;
  }
  QueuedCommand cmd;
  cmd.execute_time_ns = add_delay(current_time_ns_, config_.latency.outbound_delay_ns);
  cmd.is_cancel = true;
  cmd.command = StrategyOrderCommand{
      .client_order_id = client_order_id,
      .timestamp_ns = current_time_ns_,
  };
  pending_commands_.push_back(cmd);
}

std::optional<core::TimestampNs> OrderBroker::next_command_time() const noexcept {
  if (pending_commands_.empty()) return std::nullopt;
  auto earliest = pending_commands_.front().execute_time_ns;
  for (const auto& command : pending_commands_) {
    earliest = std::min(earliest, command.execute_time_ns);
  }
  return earliest;
}

std::optional<core::TimestampNs> OrderBroker::next_report_time() const noexcept {
  if (pending_reports_.empty()) return std::nullopt;
  auto earliest = pending_reports_.front().receipt_time_ns;
  for (const auto& report : pending_reports_) {
    earliest = std::min(earliest, report.receipt_time_ns);
  }
  return earliest;
}

void OrderBroker::process_pending_reports(const core::TimestampNs current_time_ns,
                                          Strategy& strategy,
                                          const StrategyContext& ctx) {
  core::require_valid_timestamp(current_time_ns);
  std::vector<QueuedReport> due;
  std::vector<QueuedReport> remaining;
  for (auto& queued : pending_reports_) {
    if (queued.receipt_time_ns <= current_time_ns) {
      due.push_back(std::move(queued));
    } else {
      remaining.push_back(std::move(queued));
    }
  }
  pending_reports_ = std::move(remaining);
  for (const auto& queued : due) {
    strategy.on_execution_report(ctx, queued.report, *this);
  }
}

void OrderBroker::process_external_cancellation(const core::OrderId client_order_id,
                                                const core::TimestampNs timestamp_ns) {
  core::require_valid_timestamp(timestamp_ns);
  const auto it = open_orders_map_.find(client_order_id);
  if (it == open_orders_map_.end()) return;

  StrategyExecutionReport report;
  report.client_order_id = client_order_id;
  report.timestamp_ns = timestamp_ns;
  report.side = it->second.side;
  report.order_type = StrategyOrderType::Limit;
  report.remaining_quantity = it->second.remaining_quantity;
  report.status = StrategyOrderStatus::Cancelled;
  handle_order_closed(client_order_id);
  queue_execution_report(std::move(report));
}

void OrderBroker::process_pending_commands(
    exchange::MatchingEngine& engine,
    const core::TimestampNs current_time_ns,
    Strategy& strategy,
    const StrategyContext& ctx) {
  static_cast<void>(strategy);
  static_cast<void>(ctx);
  core::require_valid_timestamp(current_time_ns);
  if (pending_commands_.empty()) return;

  std::vector<QueuedCommand> remaining;
  std::vector<QueuedCommand> to_process;
  for (auto& cmd : pending_commands_) {
    if (cmd.execute_time_ns <= current_time_ns) {
      to_process.push_back(cmd);
    } else {
      remaining.push_back(cmd);
    }
  }
  pending_commands_ = std::move(remaining);

  for (auto& queued : to_process) {
    const auto ts = queued.execute_time_ns;

    if (queued.is_cancel) {
      const auto cid = queued.command.client_order_id;
      const auto res = engine.cancel(cid, ts);
      if (res.accepted) {
        StrategyExecutionReport rep;
        rep.client_order_id = cid;
        rep.timestamp_ns = ts;
        rep.order_type = StrategyOrderType::Limit;
        if (const auto it = open_orders_map_.find(cid); it != open_orders_map_.end()) {
          rep.side = it->second.side;
          rep.remaining_quantity = it->second.remaining_quantity;
        }
        rep.status = StrategyOrderStatus::Cancelled;
        handle_order_closed(cid);
        queue_execution_report(std::move(rep));
      } else {
        ++commands_rejected_;
        StrategyExecutionReport rep;
        rep.client_order_id = cid;
        rep.timestamp_ns = ts;
        rep.status = StrategyOrderStatus::Rejected;
        rep.rejection = StrategyRejectionReason::UnknownOrder;
        queue_execution_report(std::move(rep));
      }
    } else if (queued.command.type == StrategyOrderType::Limit) {
      const auto bid = engine.book().best_bid();
      const auto ask = engine.book().best_ask();
      set_execution_reference_midprice(
          bid.has_value() && ask.has_value()
              ? std::optional<double>{(static_cast<double>(*bid) +
                                       static_cast<double>(*ask)) * 0.5}
              : std::nullopt);
      const auto cid = queued.command.client_order_id;
      const auto side = queued.command.side;
      const auto price = queued.command.price_ticks;
      const auto qty = queued.command.quantity_lots;

      const auto res = engine.submit_limit(cid, ts, side, price, qty);
      if (res.accepted) {
        // Apply any immediate crossing trades first
        for (const auto& tr : res.trades) {
          apply_fill(tr, /*is_passive=*/false, cid, side, StrategyOrderType::Limit);
        }

        // Track resting order if not completely filled immediately
        if (res.order.remaining_quantity > 0) {
          OpenStrategyOrder open_ord;
          open_ord.client_order_id = cid;
          open_ord.submission_time_ns = ts;
          open_ord.side = side;
          open_ord.price_ticks = price;
          open_ord.initial_quantity = qty;
          open_ord.remaining_quantity = res.order.remaining_quantity;
          open_orders_map_[cid] = open_ord;
          open_orders_list_.push_back(open_ord);
        }
      } else {
        ++commands_rejected_;
        StrategyExecutionReport rep;
        rep.client_order_id = cid;
        rep.timestamp_ns = ts;
        rep.side = side;
        rep.order_type = StrategyOrderType::Limit;
        rep.status = StrategyOrderStatus::Rejected;
        rep.rejection = StrategyRejectionReason::ExchangeRejection;
        queue_execution_report(std::move(rep));
      }
    } else {
      // Market order
      const auto bid = engine.book().best_bid();
      const auto ask = engine.book().best_ask();
      set_execution_reference_midprice(
          bid.has_value() && ask.has_value()
              ? std::optional<double>{(static_cast<double>(*bid) +
                                       static_cast<double>(*ask)) * 0.5}
              : std::nullopt);
      const auto cid = queued.command.client_order_id;
      const auto side = queued.command.side;
      const auto qty = queued.command.quantity_lots;

      const auto res = engine.submit_market(cid, ts, side, qty);
      if (res.accepted) {
        for (const auto& tr : res.trades) {
          apply_fill(tr, /*is_passive=*/false, cid, side, StrategyOrderType::Market);
        }
        if (res.expired_market_quantity > 0) {
          StrategyExecutionReport rep;
          rep.client_order_id = cid;
          rep.timestamp_ns = ts;
          rep.side = side;
          rep.order_type = StrategyOrderType::Market;
          rep.remaining_quantity = res.expired_market_quantity;
          rep.status = StrategyOrderStatus::Expired;
          queue_execution_report(std::move(rep));
        }
      } else {
        ++commands_rejected_;
        StrategyExecutionReport rep;
        rep.client_order_id = cid;
        rep.timestamp_ns = ts;
        rep.side = side;
        rep.order_type = StrategyOrderType::Market;
        rep.status = StrategyOrderStatus::Rejected;
        rep.rejection = StrategyRejectionReason::ExchangeRejection;
        queue_execution_report(std::move(rep));
      }
    }
  }
}

void OrderBroker::process_trade(const exchange::Trade& trade,
                                Strategy& strategy,
                                const StrategyContext& ctx) {
  static_cast<void>(strategy);
  static_cast<void>(ctx);
  auto it = open_orders_map_.find(trade.resting_order_id);
  if (it != open_orders_map_.end()) {
    const auto side = it->second.side;
    apply_fill(trade, /*is_passive=*/true, trade.resting_order_id, side,
               StrategyOrderType::Limit);
  }
}

void OrderBroker::set_execution_reference_midprice(
    const std::optional<double> midprice_ticks) {
  if (midprice_ticks.has_value() &&
      (!std::isfinite(*midprice_ticks) || *midprice_ticks < 0.0)) {
    throw std::invalid_argument{"execution reference midprice must be finite and nonnegative"};
  }
  execution_reference_midprice_ticks_ = midprice_ticks;
}

void OrderBroker::apply_fill(
    const exchange::Trade& trade,
    const bool is_passive,
    const core::OrderId client_order_id,
    const core::Side side,
    const StrategyOrderType order_type) {
  ++total_trades_;
  filled_order_ids_.insert(client_order_id);
  if (is_passive) {
    ++passive_fills_;
  } else {
    ++aggressive_fills_;
  }

  const double fill_qty = static_cast<double>(trade.quantity);
  const double fill_price = static_cast<double>(trade.price_ticks);
  const double fill_value = fill_qty * fill_price * config_.currency_per_tick;

  // Fee computation
  const double fee_per_lot = is_passive ? config_.fees.maker_fee_per_lot
                                        : config_.fees.taker_fee_per_lot;
  const double commission = commissioned_order_ids_.insert(client_order_id).second
                                ? config_.fees.commission_per_order
                                : 0.0;
  const double trade_fee = fill_qty * fee_per_lot + commission;
  portfolio_.total_fees_currency += trade_fee;
  portfolio_.gross_turnover_currency += fill_value;

  // Position & Cash update
  const double sign = (side == core::Side::Buy) ? 1.0 : -1.0;
  const double execution_cost =
      execution_reference_midprice_ticks_.has_value()
          ? sign * (fill_price - *execution_reference_midprice_ticks_) *
                fill_qty * config_.currency_per_tick
          : 0.0;
  execution_cost_currency_ += execution_cost;
  portfolio_.cash_currency -= sign * fill_value + trade_fee;

  const std::int64_t old_pos = portfolio_.position_lots;
  const std::int64_t delta_pos = static_cast<std::int64_t>(sign * fill_qty);
  const std::int64_t new_pos = old_pos + delta_pos;

  // Realized PnL via average cost basis
  if (old_pos == 0) {
    // Opening fresh position
    portfolio_.average_entry_price_ticks = fill_price;
  } else if ((old_pos > 0 && delta_pos > 0) || (old_pos < 0 && delta_pos < 0)) {
    // Adding to position in the same direction: update average entry price
    const double cur_abs = static_cast<double>(std::abs(old_pos));
    portfolio_.average_entry_price_ticks =
        (cur_abs * portfolio_.average_entry_price_ticks + fill_qty * fill_price) /
        (cur_abs + fill_qty);
  } else {
    // Closing or reducing position
    const double closing_qty = std::min(fill_qty, static_cast<double>(std::abs(old_pos)));
    const double pos_sign = (old_pos > 0) ? 1.0 : -1.0;
    // For long (pos_sign = +1): closing by selling at fill_price -> gain = fill_price - entry
    // For short (pos_sign = -1): closing by buying at fill_price -> gain = entry - fill_price
    const double trade_realized_pnl =
        pos_sign * (fill_price - portfolio_.average_entry_price_ticks) *
        closing_qty * config_.currency_per_tick;
    portfolio_.gross_realized_pnl_currency += trade_realized_pnl;

    if (new_pos == 0) {
      portfolio_.average_entry_price_ticks = 0.0;
    } else if ((old_pos > 0 && new_pos < 0) || (old_pos < 0 && new_pos > 0)) {
      // Position flipped through zero -> remaining portion establishes new entry price
      portfolio_.average_entry_price_ticks = fill_price;
    }
  }

  portfolio_.position_lots = new_pos;
  mark_to_market(last_mark_price_ticks_);
  max_inventory_exposure_lots_ = std::max(
      max_inventory_exposure_lots_,
      static_cast<std::size_t>(std::abs(portfolio_.position_lots)));

  // Update open orders tracking
  core::QuantityLots rem_qty = 0;
  if (is_passive) {
    auto it = open_orders_map_.find(client_order_id);
    if (it != open_orders_map_.end()) {
      if (it->second.remaining_quantity <= trade.quantity) {
        rem_qty = 0;
        handle_order_closed(client_order_id);
      } else {
        it->second.remaining_quantity -= trade.quantity;
        rem_qty = it->second.remaining_quantity;
        for (auto& ord : open_orders_list_) {
          if (ord.client_order_id == client_order_id) {
            ord.remaining_quantity = rem_qty;
            break;
          }
        }
      }
    }
  } else {
    rem_qty = trade.incoming_remaining_quantity;
  }

  // Create execution report
  StrategyExecutionReport rep;
  rep.client_order_id = client_order_id;
  rep.timestamp_ns = trade.timestamp_ns;
  rep.side = side;
  rep.order_type = order_type;
  rep.execution_price_ticks = trade.price_ticks;
  rep.filled_quantity = trade.quantity;
  rep.remaining_quantity = rem_qty;
  rep.fee_paid_currency = trade_fee;
  rep.execution_cost_currency = execution_cost;
  rep.is_passive = is_passive;
  rep.status = (rem_qty == 0) ? StrategyOrderStatus::Filled
                              : StrategyOrderStatus::PartiallyFilled;
  queue_execution_report(std::move(rep));
}

void OrderBroker::handle_order_closed(const core::OrderId client_order_id) {
  open_orders_map_.erase(client_order_id);
  open_orders_list_.erase(
      std::remove_if(open_orders_list_.begin(), open_orders_list_.end(),
                     [client_order_id](const OpenStrategyOrder& ord) {
                       return ord.client_order_id == client_order_id;
                     }),
      open_orders_list_.end());
}

void OrderBroker::mark_to_market(const std::optional<double> midprice_ticks) {
  if (midprice_ticks.has_value() &&
      (!std::isfinite(*midprice_ticks) || *midprice_ticks < 0.0)) {
    throw std::invalid_argument{"mark price must be finite and nonnegative"};
  }
  if (midprice_ticks.has_value()) {
    last_mark_price_ticks_ = midprice_ticks;
  }
  if (portfolio_.position_lots == 0) {
    portfolio_.unrealized_pnl_currency = 0.0;
  } else if (last_mark_price_ticks_.has_value()) {
    const double pos = static_cast<double>(portfolio_.position_lots);
    portfolio_.unrealized_pnl_currency =
        pos * (*last_mark_price_ticks_ - portfolio_.average_entry_price_ticks) *
        config_.currency_per_tick;
  }

  portfolio_.marked_equity_currency =
      portfolio_.cash_currency +
      static_cast<double>(portfolio_.position_lots) *
          last_mark_price_ticks_.value_or(0.0) * config_.currency_per_tick;
  const double equity = portfolio_.total_equity_currency();
  portfolio_.peak_equity_currency = std::max(portfolio_.peak_equity_currency, equity);
  const double current_dd = portfolio_.peak_equity_currency - equity;
  portfolio_.max_drawdown_currency = std::max(portfolio_.max_drawdown_currency, current_dd);
}

}  // namespace lob::strategy
