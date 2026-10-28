#include "lob/exchange/matching_engine.hpp"
#include "lob/simulation/simulator.hpp"
#include "lob/stochastic/hawkes_parameters.hpp"
#include "lob/stochastic/hawkes_process.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Sample final {
  std::uint64_t elapsed_ns{};
  std::size_t operations{};
  double simulated_seconds{};
  std::size_t output_bytes{};
  double checksum{};
};

template <typename Function>
void benchmark(const char* name, Function&& function) {
  constexpr std::size_t repetitions = 5U;
  std::vector<Sample> samples;
  samples.reserve(repetitions);
  for (std::size_t i = 0; i < repetitions; ++i) {
    samples.push_back(function());
  }
  std::sort(samples.begin(), samples.end(),
            [](const Sample& lhs, const Sample& rhs) {
              return lhs.elapsed_ns < rhs.elapsed_ns;
            });
  const Sample& median = samples[repetitions / 2U];
  const double seconds = static_cast<double>(median.elapsed_ns) / 1.0e9;
  std::cout << std::left << std::setw(29) << name << " events/ops="
            << median.operations << " median_ns=" << median.elapsed_ns
            << " per_op_ns="
            << (median.operations == 0U
                    ? 0.0
                    : static_cast<double>(median.elapsed_ns) /
                          static_cast<double>(median.operations))
            << " ops_per_s="
            << (seconds == 0.0
                    ? 0.0
                    : static_cast<double>(median.operations) / seconds);
  if (median.simulated_seconds > 0.0) {
    std::cout << " simulated_seconds_per_wall_second="
              << median.simulated_seconds / seconds;
  }
  if (median.output_bytes > 0U) {
    std::cout << " output_bytes=" << median.output_bytes;
  }
  std::cout << " checksum=" << median.checksum << '\n';
}

lob::exchange::Order make_limit(const lob::core::OrderId id,
                                const lob::core::Side side,
                                const lob::core::PriceTicks price) {
  return {.id = id,
          .timestamp_ns = 0,
          .side = side,
          .type = lob::exchange::OrderType::Limit,
          .price_ticks = price,
          .initial_quantity = 1U,
          .remaining_quantity = 1U,
          .status = lob::exchange::OrderStatus::New};
}

std::vector<lob::simulation::CanonicalEvent> make_events(
    const std::size_t count) {
  std::vector<lob::simulation::CanonicalEvent> events;
  events.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto id = static_cast<lob::core::OrderId>(i + 1U);
    events.push_back({.event_id = static_cast<lob::core::EventId>(i + 1U),
                      .timestamp_ns =
                          static_cast<lob::core::TimestampNs>(i * 1000U),
                      .sequence = static_cast<std::uint64_t>(i),
                      .type = lob::simulation::CommandType::Limit,
                      .order_id = id,
                      .side = lob::core::Side::Buy,
                      .price_ticks = 100,
                      .quantity = 1U});
  }
  return events;
}

}  // namespace

int main() {
  constexpr std::size_t lookup_count = 1'000'000U;
  benchmark("best-price lookup pair", [] {
    lob::exchange::OrderBook book;
    for (std::uint64_t i = 0; i < 1000U; ++i) {
      book.insert_limit(make_limit(i + 1U, lob::core::Side::Buy,
                                   5000 - static_cast<lob::core::PriceTicks>(i)));
      book.insert_limit(make_limit(i + 1001U, lob::core::Side::Sell,
                                   7000 + static_cast<lob::core::PriceTicks>(i)));
    }
    double checksum = 0.0;
    const auto started = Clock::now();
    for (std::size_t i = 0; i < lookup_count; ++i) {
      checksum += static_cast<double>(*book.best_bid());
      checksum += static_cast<double>(*book.best_ask());
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                              started)
            .count();
    return Sample{static_cast<std::uint64_t>(elapsed), lookup_count, 0.0, 0U,
                  checksum};
  });

  constexpr std::size_t order_count = 20'000U;
  benchmark("order insertion", [] {
    lob::exchange::OrderBook book;
    const auto started = Clock::now();
    for (std::uint64_t i = 0; i < order_count; ++i) {
      book.insert_limit(make_limit(
          i + 1U, lob::core::Side::Buy,
          30000 - static_cast<lob::core::PriceTicks>(i)));
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                              started)
            .count();
    return Sample{static_cast<std::uint64_t>(elapsed), order_count, 0.0, 0U,
                  static_cast<double>(book.live_order_count())};
  });

  benchmark("order cancellation", [] {
    lob::exchange::OrderBook book;
    for (std::uint64_t i = 0; i < order_count; ++i) {
      book.insert_limit(make_limit(
          i + 1U, lob::core::Side::Buy,
          30000 - static_cast<lob::core::PriceTicks>(i)));
    }
    const auto started = Clock::now();
    for (std::uint64_t i = 0; i < order_count; ++i) {
      static_cast<void>(book.remove(i + 1U));
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                              started)
            .count();
    return Sample{static_cast<std::uint64_t>(elapsed), order_count, 0.0, 0U,
                  static_cast<double>(book.live_order_count())};
  });

  benchmark("matching", [] {
    constexpr std::uint64_t resting_count = 1000U;
    lob::exchange::MatchingEngine engine;
    for (std::uint64_t i = 0; i < resting_count; ++i) {
      static_cast<void>(engine.submit_limit(i + 1U, 0, lob::core::Side::Sell,
                                            101, 1U));
    }
    const auto started = Clock::now();
    const auto result =
        engine.submit_market(resting_count + 1U, 1, lob::core::Side::Buy,
                             resting_count);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                              started)
            .count();
    return Sample{static_cast<std::uint64_t>(elapsed), resting_count, 0.0, 0U,
                  static_cast<double>(result.trades.size())};
  });

  benchmark("Hawkes intensity updates", [] {
    constexpr std::size_t updates = 100'000U;
    const lob::stochastic::HawkesParameters parameters{
        .baseline_events_per_second = {1.0, 1.0},
        .alpha_branching = {{0.1, 0.05}, {0.05, 0.1}},
        .beta_per_second = {{2.0, 2.0}, {2.0, 2.0}},
    };
    lob::stochastic::ExponentialHawkesProcess process(parameters, 42U);
    double checksum = 0.0;
    const auto started = Clock::now();
    for (std::size_t i = 0; i < updates; ++i) {
      const double time = static_cast<double>(i + 1U) * 1.0e-5;
      process.observe_event(i % 2U, time);
      checksum += process.intensities()[0];
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                              started)
            .count();
    return Sample{static_cast<std::uint64_t>(elapsed), updates, 0.0, 0U,
                  checksum};
  });

  benchmark("Hawkes event-time simulation", [] {
    const lob::stochastic::HawkesParameters parameters{
        .baseline_events_per_second = {1.0},
        .alpha_branching = {{0.3}},
        .beta_per_second = {{4.0}},
    };
    lob::stochastic::ExponentialHawkesProcess process(parameters, 20261028U);
    constexpr double horizon = 1000.0;
    const auto started = Clock::now();
    const auto events = process.simulate_until(horizon);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                              started)
            .count();
    return Sample{static_cast<std::uint64_t>(elapsed), events.size(), horizon,
                  0U, static_cast<double>(events.size())};
  });

  benchmark("event-log serialization", [] {
    const auto events = make_events(order_count);
    lob::simulation::DeterministicEventSource source(events, "benchmark");
    lob::simulation::Simulator simulator;
    const auto result = simulator.run(
        {.software_revision = "release-benchmark",
         .configuration_id = "event-log-benchmark"},
        source);
    const auto started = Clock::now();
    const auto ndjson = lob::simulation::Simulator::event_log_ndjson(result);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                              started)
            .count();
    return Sample{static_cast<std::uint64_t>(elapsed), result.dispatches.size(),
                  0.0, ndjson.size(), static_cast<double>(ndjson.size())};
  });

  benchmark("full deterministic simulation", [] {
    constexpr std::size_t count = 20'000U;
    const auto events = make_events(count);
    lob::simulation::DeterministicEventSource source(events, "benchmark");
    lob::simulation::Simulator simulator;
    const auto started = Clock::now();
    const auto result = simulator.run(
        {.software_revision = "release-benchmark",
         .configuration_id = "full-simulation-benchmark"},
        source);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                              started)
            .count();
    constexpr double interval_ns = 1000.0;
    const double simulated_seconds =
        static_cast<double>(count - 1U) * interval_ns / 1.0e9;
    return Sample{static_cast<std::uint64_t>(elapsed),
                  result.summary.events_dispatched, simulated_seconds, 0U,
                  static_cast<double>(result.summary.final_live_orders)};
  });

  return 0;
}
