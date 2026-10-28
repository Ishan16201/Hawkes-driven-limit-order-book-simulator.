#pragma once

#include <cstddef>
#include <vector>

namespace lob::stochastic {

struct HawkesParameters final {
  std::vector<double> baseline_events_per_second;
  std::vector<std::vector<double>> alpha_branching;
  std::vector<std::vector<double>> beta_per_second;
};

/// Conservative upper bound; useful diagnostically but not the stability test.
[[nodiscard]] double branching_upper_bound(const HawkesParameters& parameters);
/// Numerical spectral radius of A=alpha (row i: child type, column j: parent).
[[nodiscard]] double branching_spectral_radius(const HawkesParameters& parameters);
[[nodiscard]] std::vector<double> stationary_mean_intensity(const HawkesParameters& parameters);

/// Verifies finite nonnegative baselines/alphas, positive decays, square shape,
/// and the subcritical spectral-radius condition rho(alpha) < 1.
void validate_stationary(const HawkesParameters& parameters);

}  // namespace lob::stochastic
