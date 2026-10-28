/// Runnable synthetic-market example.
///
/// This program demonstrates the complete Hawkes-driven LOB simulation pipeline:
///   1. Configure a 6-dimensional Hawkes process with illustrative parameters.
///   2. Set up configurable mark distributions for price, size, and cancellation.
///   3. Construct an initial book with documented parameters.
///   4. Run the event loop: Hawkes → adapter → matching engine.
///   5. Print a summary report.
///
/// MODEL STATUS: All parameters are illustrative. This is not a calibrated
/// model and does not claim to resemble any real exchange. Empirical
/// validation has not been performed.

#include "lob/analytics/microstructure_analytics.hpp"
#include "lob/stochastic/hawkes_market_adapter.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void write_output_file(const std::string& path, const std::string& contents) {
  if (path.empty()) {
    return;
  }
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error{"could not open output file: " + path};
  }
  output << contents;
  if (!output) {
    throw std::runtime_error{"could not write output file: " + path};
  }
}

std::string experiment_manifest_json(
    const lob::stochastic::SyntheticMarketResult& simulation,
    const lob::analytics::AnalyticsConfig& analytics) {
  std::ostringstream out;
  out << std::setprecision(17)
      << "{\n  \"simulation\": " << simulation.run_manifest_json()
      << ",  \"analytics_configuration\": {\n"
      << "    \"currency_per_tick\": " << analytics.currency_per_tick
      << ",\n"
      << "    \"maker_fee_rebate_per_lot\": "
      << analytics.maker_fee_rebate_per_lot << ",\n"
      << "    \"taker_fee_per_lot\": " << analytics.taker_fee_per_lot
      << ",\n"
      << "    \"depth_levels\": [";
  for (std::size_t i = 0; i < analytics.depth_levels.size(); ++i) {
    if (i > 0U) out << ",";
    out << analytics.depth_levels[i];
  }
  out << "],\n    \"return_horizons_seconds\": [";
  for (std::size_t i = 0; i < analytics.return_horizons_seconds.size(); ++i) {
    if (i > 0U) out << ",";
    out << analytics.return_horizons_seconds[i];
  }
  out << "],\n    \"realized_vol_intervals_seconds\": [";
  for (std::size_t i = 0;
       i < analytics.realized_vol_intervals_seconds.size(); ++i) {
    if (i > 0U) out << ",";
    out << analytics.realized_vol_intervals_seconds[i];
  }
  out << "],\n    \"adverse_selection_horizons_seconds\": [";
  for (std::size_t i = 0;
       i < analytics.adverse_selection_horizons_seconds.size(); ++i) {
    if (i > 0U) out << ",";
    out << analytics.adverse_selection_horizons_seconds[i];
  }
  out << "]\n  }\n}\n";
  return out.str();
}

}  // namespace

int main(const int argc, char** argv) {
  std::string events_path;
  std::string trades_path;
  std::string manifest_path;
  std::string analytics_path;
  for (int i = 1; i < argc; ++i) {
    const std::string argument{argv[i]};
    if (i + 1 >= argc) {
      std::cerr << "Missing path after " << argument << '\n';
      return 2;
    }
    if (argument == "--events-out") {
      events_path = argv[++i];
    } else if (argument == "--trades-out") {
      trades_path = argv[++i];
    } else if (argument == "--manifest-out") {
      manifest_path = argv[++i];
    } else if (argument == "--analytics-out") {
      analytics_path = argv[++i];
    } else {
      std::cerr << "Unknown option: " << argument << '\n'
                << "Usage: lob_simulator [--events-out PATH] [--trades-out PATH] "
                   "[--manifest-out PATH] [--analytics-out PATH]\n";
      return 2;
    }
  }
  // -----------------------------------------------------------------------
  // Hawkes parameters (6 dimensions: LB, LA, CB, CA, MB, MS)
  //
  // Baseline intensities: events per second for each type.
  // These are illustrative values, not calibrated from data.
  // -----------------------------------------------------------------------
  const lob::stochastic::HawkesParameters hawkes_params{
      .baseline_events_per_second = {
          2.0,  // LB: limit buy arrivals
          2.0,  // LA: limit sell arrivals
          0.5,  // CB: cancel buy
          0.5,  // CA: cancel sell
          0.3,  // MB: market buy
          0.3,  // MS: market sell
      },

      // Excitation matrix alpha[i][j]: expected direct children of type i
      // from one parent event of type j. Row = child, Column = parent.
      //
      // Illustrative relationships:
      //   - Same-type self-excitation (diagonal)
      //   - Limit arrivals trigger cancellations (LB→CB, LA→CA)
      //   - Market orders trigger replenishment (MB→LB, MS→LA)
      //   - Cross-excitation between aggressive flows (MB↔MS)
      //
      // Spectral radius of this matrix must be < 1 for stationarity.
      .alpha_branching = {
          // LB    LA    CB    CA    MB    MS     <- parent
          {0.10, 0.02, 0.01, 0.01, 0.08, 0.02},  // LB child
          {0.02, 0.10, 0.01, 0.01, 0.02, 0.08},  // LA child
          {0.05, 0.01, 0.05, 0.01, 0.02, 0.01},  // CB child
          {0.01, 0.05, 0.01, 0.05, 0.01, 0.02},  // CA child
          {0.01, 0.01, 0.01, 0.01, 0.08, 0.03},  // MB child
          {0.01, 0.01, 0.01, 0.01, 0.03, 0.08},  // MS child
      },

      // Decay rates beta[i][j]: inverse seconds. Controls how quickly
      // excitation from event j on channel i fades.
      // Higher values = faster decay = more transient clustering.
      .beta_per_second = {
          {5.0, 5.0, 5.0, 5.0, 5.0, 5.0},
          {5.0, 5.0, 5.0, 5.0, 5.0, 5.0},
          {5.0, 5.0, 5.0, 5.0, 5.0, 5.0},
          {5.0, 5.0, 5.0, 5.0, 5.0, 5.0},
          {5.0, 5.0, 5.0, 5.0, 5.0, 5.0},
          {5.0, 5.0, 5.0, 5.0, 5.0, 5.0},
      },
  };

  // -----------------------------------------------------------------------
  // Mark distributions (independent of Hawkes model)
  // -----------------------------------------------------------------------
  const lob::stochastic::MarkDistributionConfig marks{
      .price_placement = {
          .reference_price_ticks = 1000,  // Fallback anchor
          .min_offset_ticks = 0,          // Can place at best
          .max_offset_ticks = 3,          // Up to 3 ticks from best
      },
      .limit_size = {
          .min_lots = 1,
          .max_lots = 5,
      },
      .market_size = {
          .min_lots = 1,
          .max_lots = 3,
      },
      .cancellation = {
          .policy = lob::stochastic::CancelTargetPolicy::BestLevelFront,
      },
  };

  // -----------------------------------------------------------------------
  // Initial book configuration
  // -----------------------------------------------------------------------
  const lob::stochastic::InitialBookConfig initial_book{
      .best_bid_ticks = 999,
      .best_ask_ticks = 1001,
      .levels_per_side = 5,
      .orders_per_level = 3,
      .tick_spacing = 1,
      .order_size = 10,
  };

  // -----------------------------------------------------------------------
  // Simulation
  // -----------------------------------------------------------------------
  const lob::stochastic::SyntheticMarketConfig config{
      .hawkes = hawkes_params,
      .seed = 20261028,
      .horizon_seconds = 10.0,  // 10-second simulation
      .marks = marks,
      .initial_book = initial_book,
  };

  lob::stochastic::HawkesMarketAdapter adapter;
  const auto result = adapter.run(config);

  // -----------------------------------------------------------------------
  // Output
  // -----------------------------------------------------------------------
  std::cout << result.summary_report() << '\n';

  // -----------------------------------------------------------------------
  // Microstructure Analytics
  // -----------------------------------------------------------------------
  lob::analytics::AnalyticsConfig analytics_config;
  analytics_config.currency_per_tick = 0.01;  // $0.01 per tick
  analytics_config.maker_fee_rebate_per_lot = -0.002;  // $0.002 rebate per lot for passive makers
  analytics_config.taker_fee_per_lot = 0.003;  // $0.003 fee per lot for aggressive takers
  analytics_config.depth_levels = {1, 5};
  analytics_config.return_horizons_seconds = {0.1, 0.5, 1.0, 5.0};
  analytics_config.realized_vol_intervals_seconds = {0.1, 1.0};
  analytics_config.adverse_selection_horizons_seconds = {0.05, 0.1, 0.5, 1.0};

  const auto analytics_report =
      lob::analytics::MicrostructureAnalytics::compute(result, analytics_config);

  write_output_file(events_path, result.event_log_ndjson());
  write_output_file(trades_path, result.trades_ndjson());
  write_output_file(manifest_path,
                    experiment_manifest_json(result, analytics_config));
  write_output_file(analytics_path, analytics_report.to_json());

  std::cout << analytics_report.to_formatted_report() << '\n';

  // Print first 5 event log entries.
  std::cout << "--- First 5 events (NDJSON) ---\n";
  const auto ndjson = result.event_log_ndjson();
  std::size_t count = 0;
  std::size_t pos = 0;
  while (pos < ndjson.size() && count < 5) {
    const auto newline = ndjson.find('\n', pos);
    if (newline == std::string::npos) {
      break;
    }
    std::cout << ndjson.substr(pos, newline - pos + 1);
    pos = newline + 1;
    ++count;
  }
  if (result.events.size() > 5) {
    std::cout << "... (" << result.events.size() - 5 << " more events)\n";
  }

  // Print machine-readable summary JSON
  std::cout << "\n--- Analytics Summary (Machine-Readable JSON) ---\n"
            << analytics_report.to_json() << '\n';

  return 0;
}
