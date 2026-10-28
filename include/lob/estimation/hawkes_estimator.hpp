#pragma once

#include "lob/estimation/types.hpp"
#include "lob/historical/types.hpp"
#include "lob/stochastic/hawkes_market_adapter.hpp"

#include <vector>

namespace lob::estimation {

class HawkesEstimator final {
 public:
  /// Computes the exact conditional log-likelihood of the observations on [0, horizon].
  [[nodiscard]] static double log_likelihood(
      const EstimationDataset& dataset,
      const stochastic::HawkesParameters& params);

  /// Computes analytic gradients of the log-likelihood with respect to (mu, alpha).
  /// For row i, gradient has dimension 1 + M: dL/d(mu_i), dL/d(alpha_i0), ..., dL/d(alpha_i,M-1).
  [[nodiscard]] static std::vector<std::vector<double>> compute_mu_alpha_gradients(
      const EstimationDataset& dataset,
      const stochastic::HawkesParameters& params);

  /// Fits an independent Poisson process baseline (closed-form MLE: mu_i = N_i / T).
  [[nodiscard]] static EstimationResult fit_poisson(
      const EstimationDataset& dataset);

  /// Performs maximum-likelihood estimation for the multivariate Hawkes process.
  [[nodiscard]] static EstimationResult fit(
      const EstimationDataset& dataset,
      const OptimizationSettings& settings = {});

  /// Constructs an EstimationDataset from mapped historical events.
  [[nodiscard]] static EstimationDataset from_historical_events(
      const std::vector<historical::MappedHistoricalEvent>& events,
      double horizon_seconds = 0.0,
      const std::string& name = "historical_events");

  /// Constructs an EstimationDataset from synthetic market events.
  [[nodiscard]] static EstimationDataset from_synthetic_events(
      const std::vector<stochastic::MarketEventRecord>& events,
      double horizon_seconds,
      const std::string& name = "synthetic_simulation");

  /// Computes the empirical Fisher Information Matrix for row i and returns standard errors.
  [[nodiscard]] static std::pair<std::vector<double>, bool> compute_standard_errors_for_row(
      const EstimationDataset& dataset,
      const stochastic::HawkesParameters& params,
      std::size_t row_i);

  /// Computes goodness-of-fit compensator residuals via the Random Time Change Theorem.
  static void evaluate_goodness_of_fit(
      const EstimationDataset& dataset,
      const stochastic::HawkesParameters& params,
      CalibrationDiagnostics& diag);
};

}  // namespace lob::estimation
