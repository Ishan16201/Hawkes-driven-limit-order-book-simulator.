#include "lob/estimation/hawkes_estimator.hpp"
#include "lob/stochastic/hawkes_process.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

using namespace lob;
using namespace lob::estimation;
using Catch::Matchers::WithinAbs;

TEST_CASE("Hawkes estimation: analytic gradient matches numerical finite differences", "[estimation]") {
  EstimationDataset dataset;
  dataset.dimension = 2;
  dataset.horizon_seconds = 10.0;
  dataset.observations.push_back({.time_seconds = 1.0, .type_index = 0});
  dataset.observations.push_back({.time_seconds = 2.0, .type_index = 1});
  dataset.observations.push_back({.time_seconds = 2.5, .type_index = 0});
  dataset.observations.push_back({.time_seconds = 4.0, .type_index = 1});
  dataset.observations.push_back({.time_seconds = 6.0, .type_index = 0});

  stochastic::HawkesParameters params;
  params.baseline_events_per_second = {0.5, 0.4};
  params.alpha_branching = {
      {0.2, 0.1},
      {0.1, 0.2},
  };
  params.beta_per_second = {
      {2.0, 2.0},
      {2.0, 2.0},
  };

  const auto analytic_grads = HawkesEstimator::compute_mu_alpha_gradients(dataset, params);

  const double eps = 1.0e-6;

  // Verify dL/dmu_0
  {
    auto p_plus = params;
    p_plus.baseline_events_per_second[0] += eps;
    auto p_minus = params;
    p_minus.baseline_events_per_second[0] -= eps;
    const double num_grad = (HawkesEstimator::log_likelihood(dataset, p_plus) -
                             HawkesEstimator::log_likelihood(dataset, p_minus)) / (2.0 * eps);
    REQUIRE_THAT(analytic_grads[0][0], WithinAbs(num_grad, 1e-4));
  }

  // Verify dL/dalpha_00
  {
    auto p_plus = params;
    p_plus.alpha_branching[0][0] += eps;
    auto p_minus = params;
    p_minus.alpha_branching[0][0] -= eps;
    const double num_grad = (HawkesEstimator::log_likelihood(dataset, p_plus) -
                             HawkesEstimator::log_likelihood(dataset, p_minus)) / (2.0 * eps);
    REQUIRE_THAT(analytic_grads[0][1], WithinAbs(num_grad, 1e-4));
  }

  // Verify dL/dalpha_01
  {
    auto p_plus = params;
    p_plus.alpha_branching[0][1] += eps;
    auto p_minus = params;
    p_minus.alpha_branching[0][1] -= eps;
    const double num_grad = (HawkesEstimator::log_likelihood(dataset, p_plus) -
                             HawkesEstimator::log_likelihood(dataset, p_minus)) / (2.0 * eps);
    REQUIRE_THAT(analytic_grads[0][2], WithinAbs(num_grad, 1e-4));
  }
}

TEST_CASE("Hawkes estimation: Poisson baseline benchmark matches analytical formula", "[estimation]") {
  EstimationDataset dataset;
  dataset.dimension = 2;
  dataset.horizon_seconds = 100.0;
  dataset.event_counts_by_type = {50, 80};

  const auto result = HawkesEstimator::fit_poisson(dataset);

  REQUIRE(result.parameters.baseline_events_per_second.size() == 2);
  REQUIRE_THAT(result.parameters.baseline_events_per_second[0], WithinAbs(50.0 / 100.0, 1e-9));
  REQUIRE_THAT(result.parameters.baseline_events_per_second[1], WithinAbs(80.0 / 100.0, 1e-9));

  // Closed form log-likelihood: 50 * ln(0.5) - 50 + 80 * ln(0.8) - 80
  const double expected_ll = (50.0 * std::log(0.5) - 50.0) + (80.0 * std::log(0.8) - 80.0);
  REQUIRE_THAT(result.diagnostics.final_log_likelihood, WithinAbs(expected_ll, 1e-6));
  REQUIRE(result.diagnostics.spectral_radius == 0.0);
  REQUIRE(result.diagnostics.is_subcritical == true);
}

TEST_CASE("Hawkes estimation: recovers plausible parameters and detects clustering over Poisson", "[estimation]") {
  // Generate synthetic Hawkes path with known parameters
  stochastic::HawkesParameters true_params;
  true_params.baseline_events_per_second = {1.0, 1.0};
  true_params.alpha_branching = {
      {0.3, 0.1},
      {0.1, 0.3},
  };
  true_params.beta_per_second = {
      {5.0, 5.0},
      {5.0, 5.0},
  };

  const double horizon = 200.0;
  stochastic::ExponentialHawkesProcess sim(true_params, /*seed=*/12345);
  const auto events = sim.simulate_until(horizon);
  REQUIRE(events.size() > 100);

  EstimationDataset dataset;
  dataset.dimension = 2;
  dataset.horizon_seconds = horizon;
  dataset.event_counts_by_type.assign(2, 0);

  for (const auto& ev : events) {
    dataset.observations.push_back({.time_seconds = ev.time_seconds, .type_index = ev.type});
    ++dataset.event_counts_by_type[ev.type];
  }

  OptimizationSettings settings;
  settings.structure = ModelStructure::GlobalDecay;
  settings.fixed_decay_per_second = 5.0;  // Known decay scale
  settings.num_restarts = 2;
  settings.max_iterations = 200;

  const auto fit_result = HawkesEstimator::fit(dataset, settings);

  // Model comparison: Hawkes must outperform Poisson on clustered data
  REQUIRE(fit_result.diagnostics.final_log_likelihood > fit_result.diagnostics.poisson_log_likelihood);
  REQUIRE(fit_result.diagnostics.lrt_statistic > 0.0);
  REQUIRE(fit_result.diagnostics.aic < (4.0 - 2.0 * fit_result.diagnostics.poisson_log_likelihood));

  // Stability constraint preserved
  REQUIRE(fit_result.diagnostics.is_subcritical == true);
  REQUIRE(fit_result.diagnostics.spectral_radius < 1.0);

  // Plausible parameter recovery: estimated baselines should be positive and close to true
  for (std::size_t i = 0; i < 2; ++i) {
    REQUIRE(fit_result.parameters.baseline_events_per_second[i] > 0.3);
    REQUIRE(fit_result.parameters.baseline_events_per_second[i] < 2.5);
  }

  // Goodness of fit: residual mean should be reasonably close to 1.0
  for (std::size_t i = 0; i < 2; ++i) {
    REQUIRE_THAT(fit_result.diagnostics.residual_mean[i], WithinAbs(1.0, 0.4));
  }

  // Formatted report and JSON generation work
  const auto report = fit_result.to_formatted_report();
  const auto json_str = fit_result.to_json();
  REQUIRE(!report.empty());
  REQUIRE(report.find("SUBCRITICAL") != std::string::npos);
  REQUIRE(!json_str.empty());
}

TEST_CASE("Hawkes estimation: handles degenerate sparse data gracefully with weak identification warning", "[estimation]") {
  EstimationDataset dataset;
  dataset.dimension = 2;
  dataset.horizon_seconds = 10.0;
  // Only 1 observation on type 0, none on type 1
  dataset.observations.push_back({.time_seconds = 2.0, .type_index = 0});
  dataset.event_counts_by_type = {1, 0};

  OptimizationSettings settings;
  settings.structure = ModelStructure::GlobalDecay;
  settings.fixed_decay_per_second = 5.0;

  const auto fit_result = HawkesEstimator::fit(dataset, settings);

  // Weak identification is detected and reported
  REQUIRE(fit_result.diagnostics.weak_identification_detected == true);
  REQUIRE(!fit_result.diagnostics.warnings.empty());
}

TEST_CASE("empty-history likelihood includes the full baseline compensator") {
  EstimationDataset dataset;
  dataset.dimension = 1U;
  dataset.horizon_seconds = 10.0;
  const stochastic::HawkesParameters params{
      .baseline_events_per_second = {2.0},
      .alpha_branching = {{0.0}},
      .beta_per_second = {{1.0}},
  };
  REQUIRE_THAT(HawkesEstimator::log_likelihood(dataset, params),
               WithinAbs(-20.0, 1.0e-12));
  const auto poisson = HawkesEstimator::fit_poisson(dataset);
  REQUIRE_THAT(poisson.diagnostics.final_log_likelihood,
               WithinAbs(-1.0e-7, 1.0e-12));
}

TEST_CASE("time-change diagnostics use two-sided KS and mark undersampled channels unavailable") {
  EstimationDataset dataset;
  dataset.dimension = 2U;
  dataset.horizon_seconds = 5.0;
  dataset.observations = {{.time_seconds = 1.0, .type_index = 0U},
                          {.time_seconds = 3.0, .type_index = 0U},
                          {.time_seconds = 4.0, .type_index = 1U}};
  const stochastic::HawkesParameters params{
      .baseline_events_per_second = {1.0, 1.0},
      .alpha_branching = {{0.0, 0.0}, {0.0, 0.0}},
      .beta_per_second = {{1.0, 1.0}, {1.0, 1.0}},
  };

  CalibrationDiagnostics diagnostics;
  HawkesEstimator::evaluate_goodness_of_fit(dataset, params, diagnostics);
  REQUIRE(diagnostics.residual_count == std::vector<std::size_t>{2U, 1U});
  REQUIRE_THAT(diagnostics.residual_mean[0], WithinAbs(1.5, 1.0e-12));
  REQUIRE_THAT(diagnostics.residual_variance[0], WithinAbs(0.5, 1.0e-12));
  REQUIRE_THAT(diagnostics.ks_statistic[0],
               WithinAbs(1.0 - std::exp(-1.0), 1.0e-12));
  REQUIRE(diagnostics.residual_mean[1] == 0.0);
  REQUIRE(diagnostics.residual_variance[1] == 0.0);
  REQUIRE(diagnostics.ks_statistic[1] == 0.0);
}

TEST_CASE("unsupported decay structure and inconsistent reduced dimensions fail explicitly") {
  EstimationDataset dataset;
  dataset.dimension = 2U;
  dataset.horizon_seconds = 10.0;
  OptimizationSettings settings;
  settings.fixed_decay_per_second = 2.0;
  settings.structure = ModelStructure::SharedParentDecay;
  REQUIRE_THROWS_AS(HawkesEstimator::fit(dataset, settings),
                    std::invalid_argument);

  settings.structure = ModelStructure::Univariate1D;
  REQUIRE_THROWS_AS(HawkesEstimator::fit(dataset, settings),
                    std::invalid_argument);
}

TEST_CASE("fixed-decay univariate parameters recover across synthetic seeds") {
  const stochastic::HawkesParameters truth{
      .baseline_events_per_second = {0.7},
      .alpha_branching = {{0.25}},
      .beta_per_second = {{3.0}},
  };
  constexpr std::size_t run_count = 5U;
  constexpr double horizon = 5000.0;
  std::vector<double> baseline_estimates;
  std::vector<double> alpha_estimates;
  std::vector<double> beta_misspecification_log_likelihood_differences;

  for (std::uint64_t seed = 10U; seed < 10U + run_count; ++seed) {
    stochastic::ExponentialHawkesProcess process(truth, seed);
    const auto events = process.simulate_until(horizon);
    EstimationDataset dataset;
    dataset.dimension = 1U;
    dataset.horizon_seconds = horizon;
    dataset.event_counts_by_type = {0U};
    dataset.observations.reserve(events.size());
    for (const auto& event : events) {
      dataset.observations.push_back(
          {.time_seconds = event.time_seconds, .type_index = 0U});
      ++dataset.event_counts_by_type[0];
    }
    auto misspecified = truth;
    misspecified.beta_per_second[0][0] = 0.5;
    beta_misspecification_log_likelihood_differences.push_back(
        HawkesEstimator::log_likelihood(dataset, truth) -
        HawkesEstimator::log_likelihood(dataset, misspecified));

    OptimizationSettings settings;
    settings.structure = ModelStructure::Univariate1D;
    settings.fixed_decay_per_second = 3.0;
    settings.num_restarts = 2U;
    settings.max_iterations = 500U;
    settings.seed = seed;
    const auto fit = HawkesEstimator::fit(dataset, settings);
    baseline_estimates.push_back(
        fit.parameters.baseline_events_per_second[0]);
    alpha_estimates.push_back(fit.parameters.alpha_branching[0][0]);
  }

  const auto summarize = [](const std::vector<double>& values) {
    const double mean =
        std::accumulate(values.begin(), values.end(), 0.0) /
        static_cast<double>(values.size());
    double sum_squares = 0.0;
    for (const double value : values) {
      const double deviation = value - mean;
      sum_squares += deviation * deviation;
    }
    const double standard_error =
        std::sqrt(sum_squares /
                  static_cast<double>(values.size() - 1U)) /
        std::sqrt(static_cast<double>(values.size()));
    return std::pair{mean, standard_error};
  };
  const auto [baseline_mean, baseline_se] = summarize(baseline_estimates);
  const auto [alpha_mean, alpha_se] = summarize(alpha_estimates);
  const auto [likelihood_difference_mean, likelihood_difference_se] =
      summarize(beta_misspecification_log_likelihood_differences);

  REQUIRE(std::abs(baseline_mean - truth.baseline_events_per_second[0]) <=
          3.0 * baseline_se);
  REQUIRE(std::abs(alpha_mean - truth.alpha_branching[0][0]) <=
          3.0 * alpha_se);
  REQUIRE(likelihood_difference_mean > 3.0 * likelihood_difference_se);
}
