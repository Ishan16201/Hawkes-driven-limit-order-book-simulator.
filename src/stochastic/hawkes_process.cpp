#include "lob/stochastic/hawkes_process.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace lob::stochastic {

ExponentialHawkesProcess::ExponentialHawkesProcess(
    HawkesParameters p, const std::uint64_t seed)
    : parameters_(std::move(p)),
      state_(parameters_.baseline_events_per_second.size(),
             std::vector<double>(parameters_.baseline_events_per_second.size(),
                                 0.0)),
      rng_(seed) {
  validate_stationary(parameters_);
}

double ExponentialHawkesProcess::uniform_open() {
  constexpr double scale = 0x1.0p-51;
  return (static_cast<double>(rng_() >> 13U) + 0.5) * scale;
}

void ExponentialHawkesProcess::advance_to(const double t) {
  if (!std::isfinite(t) || t < time_seconds_) {
    throw std::invalid_argument{"invalid Hawkes time advance"};
  }
  const double dt = t - time_seconds_;
  for (std::size_t i = 0; i < state_.size(); ++i) {
    for (std::size_t j = 0; j < state_.size(); ++j) {
      state_[i][j] *= std::exp(-parameters_.beta_per_second[i][j] * dt);
    }
  }
  time_seconds_ = t;
}

std::vector<double> ExponentialHawkesProcess::intensities() const {
  std::vector<double> r = parameters_.baseline_events_per_second;
  for (std::size_t i = 0; i < r.size(); ++i) {
    for (double x : state_[i]) {
      r[i] += x;
      if (!std::isfinite(r[i]) || r[i] < 0.0) {
        throw std::overflow_error{"invalid Hawkes intensity"};
      }
    }
  }
  return r;
}

void ExponentialHawkesProcess::observe_event(const std::size_t k,
                                             const double t) {
  if (k >= state_.size()) {
    throw std::invalid_argument{"invalid Hawkes event type"};
  }
  advance_to(t);
  for (std::size_t i = 0; i < state_.size(); ++i) {
    state_[i][k] +=
        parameters_.alpha_branching[i][k] * parameters_.beta_per_second[i][k];
  }
}

std::vector<HawkesEvent> ExponentialHawkesProcess::simulate_until(
    const double horizon) {
  if (!std::isfinite(horizon) || horizon < time_seconds_) {
    throw std::invalid_argument{"invalid Hawkes horizon"};
  }

  std::vector<HawkesEvent> events;

  while (time_seconds_ < horizon) {
    const auto upper = intensities();
    double total = 0.0;
    for (double v : upper) {
      total += v;
    }
    if (!std::isfinite(total) || total <= 0.0) {
      throw std::overflow_error{"invalid total intensity"};
    }

    const double candidate =
        time_seconds_ - std::log(uniform_open()) / total;
    if (candidate > horizon) {
      advance_to(horizon);
      break;
    }

    advance_to(candidate);
    const auto current = intensities();
    double actual = 0.0;
    for (double v : current) {
      actual += v;
    }
    if (uniform_open() * total > actual) {
      continue;  // Thinned: reject this proposal.
    }

    // Accept: choose event type proportional to intensities.
    double draw = uniform_open() * actual;
    std::size_t type = 0;
    for (; type + 1U < current.size(); ++type) {
      if (draw < current[type]) {
        break;
      }
      draw -= current[type];
    }

    events.push_back({.time_seconds = time_seconds_,
                      .type = type,
                      .pre_event_intensities = current});
    observe_event(type, time_seconds_);
  }

  return events;
}

}  // namespace lob::stochastic
