#pragma once

#include "lob/stochastic/hawkes_parameters.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace lob::stochastic {

/// A single event produced by the Hawkes process.
struct HawkesEvent final {
  /// Event time in seconds from simulation origin.
  double time_seconds{};

  /// Event type as a dimension index (0..N-1).
  /// Use event_type_from_index() to convert to the EventType enum.
  std::size_t type{};

  /// Intensities at the event time, measured *before* the event's jump.
  std::vector<double> pre_event_intensities;
};

/// Exact exponential state for a multivariate Hawkes process.
///
/// At current time t, state_[i][j] is the post-history contribution of
/// parent type j to child intensity i. Intensities queried at an event
/// time are pre-event; observe_event then applies that event's jump.
///
/// The generator uses Ogata thinning with exact exponential decay updates.
/// A wrapped deterministic std::mt19937_64 provides all randomness.
class ExponentialHawkesProcess final {
 public:
  /// Construct with validated parameters and a deterministic seed.
  /// Throws std::invalid_argument if parameters fail validation.
  explicit ExponentialHawkesProcess(HawkesParameters parameters,
                                   std::uint64_t seed);

  /// Decay all state to the given time. Must not go backward.
  void advance_to(double time_seconds);

  /// Record an event of the given type at the given time.
  /// Updates the excitation state. Type must be < dimension.
  void observe_event(std::size_t type, double time_seconds);

  /// Current intensities (baseline + all excitation contributions).
  [[nodiscard]] std::vector<double> intensities() const;

  /// Current process time.
  [[nodiscard]] double current_time() const noexcept { return time_seconds_; }

  /// Dimension of the process (number of event types).
  [[nodiscard]] std::size_t dimension() const noexcept {
    return parameters_.baseline_events_per_second.size();
  }

  /// Generate all events up to the horizon using Ogata thinning.
  /// The process state is advanced to exactly the horizon on return.
  [[nodiscard]] std::vector<HawkesEvent> simulate_until(
      double horizon_seconds);

 private:
  [[nodiscard]] double uniform_open();

  HawkesParameters parameters_;
  std::vector<std::vector<double>> state_;
  double time_seconds_{};
  std::mt19937_64 rng_;
};

}  // namespace lob::stochastic
