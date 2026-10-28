#pragma once

#include "lob/stochastic/event_types.hpp"
#include "lob/stochastic/hawkes_parameters.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lob::estimation {

/// A single timestamped event observation used for calibration.
struct HawkesObservation final {
  /// Event timestamp in seconds from observation origin.
  double time_seconds{0.0};
  /// Event type index in [0, dimension - 1].
  std::size_t type_index{0};
};

/// Dataset of event occurrences over an observation window [0, horizon_seconds].
struct EstimationDataset final {
  std::string dataset_name{"synthetic"};
  std::string provenance{"internal_simulation"};
  double horizon_seconds{0.0};
  std::size_t dimension{stochastic::kEventTypeCount};
  std::vector<HawkesObservation> observations;
  std::vector<std::size_t> event_counts_by_type;
  std::vector<std::string> type_labels;

  [[nodiscard]] std::size_t total_events() const noexcept {
    return observations.size();
  }
};

/// Estimation model structure / parameter restriction.
enum class ModelStructure : std::uint8_t {
  /// Full branching matrix; decay is fixed or profiled as one common value.
  FullMultivariate,
  /// Reserved; parent-specific decay profiling is not implemented.
  SharedParentDecay,
  /// Full branching matrix with one common decay rate.
  GlobalDecay,
  /// Diagonal self-excitation only (cross-alphas = 0).
  DiagonalSelfExcitation,
  /// Bivariate model (e.g. Buy vs Sell flow, dimension = 2).
  Bivariate2D,
  /// Univariate model (aggregate order flow, dimension = 1).
  Univariate1D,
  /// Independent Poisson benchmark (alpha = 0).
  IndependentPoisson,
};

/// Numerical optimization settings for Hawkes MLE.
struct OptimizationSettings final {
  ModelStructure structure{ModelStructure::GlobalDecay};

  /// Number of random initializations for multi-start optimization.
  std::size_t num_restarts{3};

  /// Maximum projected-gradient iterations per start.
  std::size_t max_iterations{300};

  /// Convergence tolerance on the unscaled log-likelihood gradient norm.
  double tolerance{1.0e-6};

  /// Optional fixed decay parameter beta. If provided, estimation is strictly
  /// convex in (mu, alpha), guaranteeing unique global optimum.
  std::optional<double> fixed_decay_per_second{};

  /// Maximum allowed spectral radius rho(A) for subcritical stability.
  double max_spectral_radius{0.999};

  /// RNG seed for deterministic restarts.
  std::uint64_t seed{42};
};

/// Detailed diagnostics of the calibration procedure and model fit.
struct CalibrationDiagnostics final {
  bool converged{false};
  std::size_t iterations_used{0};
  std::size_t function_evaluations{0};
  double final_log_likelihood{0.0};
  double poisson_log_likelihood{0.0};

  /// Likelihood Ratio Test statistic: 2 * (ln L_hawkes - ln L_poisson).
  double lrt_statistic{0.0};
  double lrt_p_value{1.0};
  std::size_t degrees_of_freedom{0};

  double aic{0.0};
  double bic{0.0};
  double gradient_norm{0.0};

  /// Stability diagnostics
  double spectral_radius{0.0};
  bool is_subcritical{true};
  std::vector<double> stationary_mean;

  /// Fisher information / Hessian condition diagnostics
  bool weak_identification_detected{false};
  double max_hessian_condition_number{0.0};

  /// Goodness-of-fit via Random Time Change Theorem:
  /// Compensator inter-arrival residuals should have mean = 1 and variance = 1.
  std::vector<std::size_t> residual_count;
  std::vector<double> residual_mean;
  std::vector<double> residual_variance;
  std::vector<double> ks_statistic;

  std::vector<std::string> warnings;
};

/// Complete result of parameter estimation.
struct EstimationResult final {
  stochastic::HawkesParameters parameters;
  stochastic::HawkesParameters parameter_standard_errors;
  CalibrationDiagnostics diagnostics;
  OptimizationSettings settings;
  std::string dataset_name;
  double horizon_seconds{0.0};
  std::size_t total_events{0};

  [[nodiscard]] std::string to_formatted_report() const;
  [[nodiscard]] std::string to_json() const;
};

}  // namespace lob::estimation
