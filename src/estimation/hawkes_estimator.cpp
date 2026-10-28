#include "lob/estimation/hawkes_estimator.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>

namespace lob::estimation {
namespace {

void validate_dataset(const EstimationDataset& dataset) {
  if (dataset.dimension == 0U || !std::isfinite(dataset.horizon_seconds) ||
      dataset.horizon_seconds <= 0.0) {
    throw std::invalid_argument{"estimation dataset requires a positive finite horizon and dimension"};
  }
  if (!dataset.event_counts_by_type.empty() &&
      dataset.event_counts_by_type.size() != dataset.dimension) {
    throw std::invalid_argument{"event-count vector dimension does not match dataset"};
  }

  double previous_time = -1.0;
  for (const auto& event : dataset.observations) {
    if (!std::isfinite(event.time_seconds) || event.time_seconds < 0.0 ||
        event.time_seconds > dataset.horizon_seconds ||
        event.time_seconds < previous_time ||
        event.type_index >= dataset.dimension) {
      throw std::invalid_argument{"invalid or unordered Hawkes observation"};
    }
    previous_time = event.time_seconds;
  }

  if (!dataset.observations.empty() &&
      !dataset.event_counts_by_type.empty()) {
    std::vector<std::size_t> observed_counts(dataset.dimension, 0U);
    for (const auto& event : dataset.observations) {
      ++observed_counts[event.type_index];
    }
    if (observed_counts != dataset.event_counts_by_type) {
      throw std::invalid_argument{"event counts do not match Hawkes observations"};
    }
  }
}

void validate_parameter_shapes(const stochastic::HawkesParameters& params,
                               const std::size_t dimension) {
  if (params.baseline_events_per_second.size() != dimension ||
      params.alpha_branching.size() != dimension ||
      params.beta_per_second.size() != dimension) {
    throw std::invalid_argument{"Hawkes parameter dimensions do not match dataset"};
  }
  for (std::size_t i = 0; i < dimension; ++i) {
    if (params.alpha_branching[i].size() != dimension ||
        params.beta_per_second[i].size() != dimension ||
        !std::isfinite(params.baseline_events_per_second[i]) ||
        params.baseline_events_per_second[i] <= 0.0) {
      throw std::invalid_argument{"invalid Hawkes parameter dimensions or baseline"};
    }
    for (std::size_t j = 0; j < dimension; ++j) {
      if (!std::isfinite(params.alpha_branching[i][j]) ||
          params.alpha_branching[i][j] < 0.0 ||
          !std::isfinite(params.beta_per_second[i][j]) ||
          params.beta_per_second[i][j] <= 0.0) {
        throw std::invalid_argument{"invalid Hawkes branching or decay parameter"};
      }
    }
  }
}

std::vector<std::size_t> event_counts(const EstimationDataset& dataset) {
  if (!dataset.event_counts_by_type.empty()) {
    return dataset.event_counts_by_type;
  }
  std::vector<std::size_t> counts(dataset.dimension, 0U);
  for (const auto& event : dataset.observations) {
    ++counts[event.type_index];
  }
  return counts;
}

/// Recursive memory state for evaluating intensities.
/// R[i][j] stores sum_{l < k, m_l = j} exp(-beta_{ij} * (t_k - t_l)).
struct IntensityState {
  std::vector<std::vector<double>> R;
  double last_time{0.0};

  explicit IntensityState(std::size_t dim)
      : R(dim, std::vector<double>(dim, 0.0)) {}

  void update_to(double current_time, std::size_t prev_type,
                 const std::vector<std::vector<double>>& beta) {
    const double dt = current_time - last_time;
    if (dt < 0.0) return;
    for (std::size_t i = 0; i < R.size(); ++i) {
      for (std::size_t j = 0; j < R.size(); ++j) {
        R[i][j] = std::exp(-beta[i][j] * dt) *
                  (R[i][j] + (prev_type == j ? 1.0 : 0.0));
      }
    }
    last_time = current_time;
  }
};

/// Chi-squared upper tail approximation (Wilson-Hilferty / asymptotic).
double chi2_p_value(double chi2, double df) {
  if (chi2 <= 0.0 || df <= 0.0) return 1.0;
  // Wilson-Hilferty transformation to normal z-score
  const double term = std::cbrt(chi2 / df);
  const double mean = 1.0 - 2.0 / (9.0 * df);
  const double var = 2.0 / (9.0 * df);
  if (var <= 0.0) return 1.0;
  const double z = (term - mean) / std::sqrt(var);
  // Standard normal CDF approximation
  const double p = 0.5 * std::erfc(z / std::sqrt(2.0));
  return std::max(0.0, std::min(1.0, p));
}

/// Invert an (M+1)x(M+1) symmetric positive semi-definite matrix using Gauss-Jordan with pivoting.
/// Returns false if singular or condition number is too large (weak identification).
bool invert_matrix(const std::vector<std::vector<double>>& A,
                   std::vector<std::vector<double>>& inv,
                   double& condition_diag) {
  const std::size_t n = A.size();
  inv.assign(n, std::vector<double>(n, 0.0));
  for (std::size_t i = 0; i < n; ++i) inv[i][i] = 1.0;

  std::vector<std::vector<double>> mat = A;
  double max_diag = 0.0;
  double min_diag = std::numeric_limits<double>::infinity();

  for (std::size_t i = 0; i < n; ++i) {
    max_diag = std::max(max_diag, std::abs(mat[i][i]));
    min_diag = std::min(min_diag, std::abs(mat[i][i]));
  }
  condition_diag = (min_diag > 1.0e-15) ? (max_diag / min_diag) : 1.0e16;

  for (std::size_t i = 0; i < n; ++i) {
    // Find pivot
    std::size_t pivot = i;
    for (std::size_t r = i + 1; r < n; ++r) {
      if (std::abs(mat[r][i]) > std::abs(mat[pivot][i])) {
        pivot = r;
      }
    }
    if (std::abs(mat[pivot][i]) < 1.0e-12) {
      return false;  // Singular
    }
    std::swap(mat[i], mat[pivot]);
    std::swap(inv[i], inv[pivot]);

    const double div = mat[i][i];
    for (std::size_t j = 0; j < n; ++j) {
      mat[i][j] /= div;
      inv[i][j] /= div;
    }
    for (std::size_t r = 0; r < n; ++r) {
      if (r != i) {
        const double factor = mat[r][i];
        for (std::size_t c = 0; c < n; ++c) {
          mat[r][c] -= factor * mat[i][c];
          inv[r][c] -= factor * inv[i][c];
        }
      }
    }
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Likelihood & Gradients
// ---------------------------------------------------------------------------

double HawkesEstimator::log_likelihood(
    const EstimationDataset& dataset,
    const stochastic::HawkesParameters& params) {
  validate_dataset(dataset);
  validate_parameter_shapes(params, dataset.dimension);
  const auto dim = dataset.dimension;
  const auto& obs = dataset.observations;
  const double T = dataset.horizon_seconds;

  // Pre-calculate compensator integrals:
  // Lambda_i(T) = mu_i * T + sum_j alpha_ij * sum_{l: m_l=j} (1 - exp(-beta_ij * (T - t_l)))
  std::vector<double> compensator(dim, 0.0);
  for (std::size_t i = 0; i < dim; ++i) {
    compensator[i] = params.baseline_events_per_second[i] * T;
  }

  for (const auto& ev : obs) {
    const std::size_t j = ev.type_index;
    const double rem_time = T - ev.time_seconds;
    if (rem_time >= 0.0) {
      for (std::size_t i = 0; i < dim; ++i) {
        compensator[i] += params.alpha_branching[i][j] *
                          (1.0 - std::exp(-params.beta_per_second[i][j] * rem_time));
      }
    }
  }

  // Event log-intensity sum: sum_k ln lambda_{m_k}(t_k)
  double log_sum = 0.0;
  IntensityState state(dim);
  std::size_t last_type = 0;
  bool is_first = true;

  for (const auto& ev : obs) {
    if (!is_first) {
      state.update_to(ev.time_seconds, last_type, params.beta_per_second);
    } else {
      state.last_time = ev.time_seconds;
      is_first = false;
    }

    const std::size_t i = ev.type_index;
    double lambda_i = params.baseline_events_per_second[i];
    for (std::size_t j = 0; j < dim; ++j) {
      lambda_i += params.alpha_branching[i][j] *
                  params.beta_per_second[i][j] * state.R[i][j];
    }

    if (lambda_i <= 0.0 || !std::isfinite(lambda_i)) {
      return -std::numeric_limits<double>::infinity();
    }
    log_sum += std::log(lambda_i);
    last_type = ev.type_index;
  }

  double total_compensator = 0.0;
  for (std::size_t i = 0; i < dim; ++i) {
    total_compensator += compensator[i];
  }

  return log_sum - total_compensator;
}

std::vector<std::vector<double>> HawkesEstimator::compute_mu_alpha_gradients(
    const EstimationDataset& dataset,
    const stochastic::HawkesParameters& params) {
  validate_dataset(dataset);
  validate_parameter_shapes(params, dataset.dimension);
  const auto dim = dataset.dimension;
  const auto& obs = dataset.observations;
  const double T = dataset.horizon_seconds;

  // Gradients for each row i: grad[i][0] is dL/dmu_i, grad[i][1+j] is dL/dalpha_ij
  std::vector<std::vector<double>> grads(dim, std::vector<double>(1 + dim, 0.0));

  // Subtract compensator derivatives:
  // dLambda_i / dmu_i = T
  // dLambda_i / dalpha_ij = sum_{l: m_l=j} (1 - exp(-beta_ij * (T - t_l)))
  for (std::size_t i = 0; i < dim; ++i) {
    grads[i][0] = -T;
  }

  for (const auto& ev : obs) {
    const std::size_t j = ev.type_index;
    const double rem_time = T - ev.time_seconds;
    if (rem_time >= 0.0) {
      for (std::size_t i = 0; i < dim; ++i) {
        grads[i][1 + j] -= (1.0 - std::exp(-params.beta_per_second[i][j] * rem_time));
      }
    }
  }

  // Add event derivatives:
  // d ln lambda_i(t_k) / dmu_i = 1 / lambda_i(t_k)
  // d ln lambda_i(t_k) / dalpha_ij = (beta_ij * R_ij(t_k)) / lambda_i(t_k)
  IntensityState state(dim);
  std::size_t last_type = 0;
  bool is_first = true;

  for (const auto& ev : obs) {
    if (!is_first) {
      state.update_to(ev.time_seconds, last_type, params.beta_per_second);
    } else {
      state.last_time = ev.time_seconds;
      is_first = false;
    }

    const std::size_t i = ev.type_index;
    double lambda_i = params.baseline_events_per_second[i];
    for (std::size_t j = 0; j < dim; ++j) {
      lambda_i += params.alpha_branching[i][j] *
                  params.beta_per_second[i][j] * state.R[i][j];
    }

    if (lambda_i > 0.0) {
      const double inv_lambda = 1.0 / lambda_i;
      grads[i][0] += inv_lambda;
      for (std::size_t j = 0; j < dim; ++j) {
        grads[i][1 + j] += (params.beta_per_second[i][j] * state.R[i][j]) * inv_lambda;
      }
    }
    last_type = ev.type_index;
  }

  return grads;
}

// ---------------------------------------------------------------------------
// Poisson Benchmark
// ---------------------------------------------------------------------------

EstimationResult HawkesEstimator::fit_poisson(const EstimationDataset& dataset) {
  validate_dataset(dataset);
  EstimationResult result;
  result.dataset_name = dataset.dataset_name;
  result.horizon_seconds = dataset.horizon_seconds;
  result.total_events = dataset.total_events();
  result.settings.structure = ModelStructure::IndependentPoisson;
  const auto dim = dataset.dimension;

  result.parameters.baseline_events_per_second.assign(dim, 0.0);
  result.parameters.alpha_branching.assign(dim, std::vector<double>(dim, 0.0));
  result.parameters.beta_per_second.assign(dim, std::vector<double>(dim, 1.0));

  result.parameter_standard_errors.baseline_events_per_second.assign(dim, 0.0);
  result.parameter_standard_errors.alpha_branching.assign(dim, std::vector<double>(dim, 0.0));
  result.parameter_standard_errors.beta_per_second.assign(dim, std::vector<double>(dim, 0.0));

  double log_lik = 0.0;
  const double T = dataset.horizon_seconds;
  const auto counts = event_counts(dataset);

  for (std::size_t i = 0; i < dim; ++i) {
    const double n_i = static_cast<double>(counts[i]);
    const double mu_hat = std::max(1.0e-8, n_i / T);
    result.parameters.baseline_events_per_second[i] = mu_hat;
    result.parameter_standard_errors.baseline_events_per_second[i] =
        (n_i > 0.0) ? (std::sqrt(n_i) / T) : 0.0;

    log_lik += n_i * std::log(mu_hat) - mu_hat * T;
  }

  result.diagnostics.converged = true;
  result.diagnostics.final_log_likelihood = log_lik;
  result.diagnostics.poisson_log_likelihood = log_lik;
  result.diagnostics.lrt_statistic = 0.0;
  result.diagnostics.lrt_p_value = 1.0;
  result.diagnostics.aic = 2.0 * static_cast<double>(dim) - 2.0 * log_lik;
  result.diagnostics.bic = static_cast<double>(dim) * std::log(static_cast<double>(std::max<std::size_t>(1, dataset.total_events()))) -
                           2.0 * log_lik;
  result.diagnostics.spectral_radius = 0.0;
  result.diagnostics.is_subcritical = true;
  result.diagnostics.stationary_mean = result.parameters.baseline_events_per_second;

  return result;
}

// ---------------------------------------------------------------------------
// Fisher Information & Standard Errors
// ---------------------------------------------------------------------------

std::pair<std::vector<double>, bool>
HawkesEstimator::compute_standard_errors_for_row(
    const EstimationDataset& dataset,
    const stochastic::HawkesParameters& params,
    const std::size_t row_i) {
  const auto dim = dataset.dimension;
  const auto& obs = dataset.observations;

  // Outer product of score vectors for row i:
  // J_i = sum_{k: m_k = i} v_k v_k^T / lambda_i(t_k)^2
  // where v_k = (1, beta_i0 * R_i0(t_k), ..., beta_i,M-1 * R_i,M-1(t_k))^T
  const std::size_t p = 1 + dim;
  std::vector<std::vector<double>> fisher(p, std::vector<double>(p, 0.0));

  IntensityState state(dim);
  std::size_t last_type = 0;
  bool is_first = true;
  std::size_t events_on_row = 0;

  for (const auto& ev : obs) {
    if (!is_first) {
      state.update_to(ev.time_seconds, last_type, params.beta_per_second);
    } else {
      state.last_time = ev.time_seconds;
      is_first = false;
    }

    if (ev.type_index == row_i) {
      ++events_on_row;
      double lambda_i = params.baseline_events_per_second[row_i];
      std::vector<double> v(p, 0.0);
      v[0] = 1.0;
      for (std::size_t j = 0; j < dim; ++j) {
        const double term = params.beta_per_second[row_i][j] * state.R[row_i][j];
        v[1 + j] = term;
        lambda_i += params.alpha_branching[row_i][j] * term;
      }

      if (lambda_i > 0.0) {
        const double w = 1.0 / (lambda_i * lambda_i);
        for (std::size_t r = 0; r < p; ++r) {
          for (std::size_t c = 0; c < p; ++c) {
            fisher[r][c] += w * v[r] * v[c];
          }
        }
      }
    }
    last_type = ev.type_index;
  }

  std::vector<double> ses(p, 0.0);
  if (events_on_row < p) {
    // Insufficient events on this channel to identify all parameters
    return {ses, false};
  }

  std::vector<std::vector<double>> inv_fisher;
  double condition = 0.0;
  if (!invert_matrix(fisher, inv_fisher, condition)) {
    return {ses, false};
  }

  for (std::size_t r = 0; r < p; ++r) {
    ses[r] = (inv_fisher[r][r] > 0.0) ? std::sqrt(inv_fisher[r][r]) : 0.0;
  }

  const bool well_identified = (condition < 1.0e10);
  return {ses, well_identified};
}

// ---------------------------------------------------------------------------
// Goodness-of-Fit (Time-Change Theorem)
// ---------------------------------------------------------------------------

void HawkesEstimator::evaluate_goodness_of_fit(
    const EstimationDataset& dataset,
    const stochastic::HawkesParameters& params,
    CalibrationDiagnostics& diag) {
  validate_dataset(dataset);
  validate_parameter_shapes(params, dataset.dimension);
  const auto dim = dataset.dimension;
  const auto& obs = dataset.observations;

  diag.residual_count.assign(dim, 0U);
  diag.residual_mean.assign(dim, 0.0);
  diag.residual_variance.assign(dim, 0.0);
  diag.ks_statistic.assign(dim, 0.0);

  std::vector<double> last_compensator(dim, 0.0);
  std::vector<std::vector<double>> residuals(dim);

  // Track integrated intensity Lambda_i(t) as time advances
  IntensityState state(dim);
  std::size_t last_type = 0;
  bool is_first = true;
  double cur_t = 0.0;
  std::vector<double> accumulated_integral(dim, 0.0);

  for (const auto& ev : obs) {
    const double dt = ev.time_seconds - cur_t;
    if (dt > 0.0) {
      // Analytical integral of lambda_i from cur_t to ev.time_seconds:
      // mu_i * dt + sum_j alpha_ij * R_ij * (1 - exp(-beta_ij * dt))
      for (std::size_t i = 0; i < dim; ++i) {
        double int_val = params.baseline_events_per_second[i] * dt;
        for (std::size_t j = 0; j < dim; ++j) {
          int_val += params.alpha_branching[i][j] * state.R[i][j] *
                     (1.0 - std::exp(-params.beta_per_second[i][j] * dt));
        }
        accumulated_integral[i] += int_val;
      }
    }

    if (!is_first) {
      state.update_to(ev.time_seconds, last_type, params.beta_per_second);
    } else {
      state.last_time = ev.time_seconds;
      is_first = false;
    }
    cur_t = ev.time_seconds;
    last_type = ev.type_index;

    const std::size_t i = ev.type_index;
    const double res = accumulated_integral[i] - last_compensator[i];
    if (res > 0.0) {
      residuals[i].push_back(res);
      last_compensator[i] = accumulated_integral[i];
    }
  }

  for (std::size_t i = 0; i < dim; ++i) {
    const auto& res = residuals[i];
    diag.residual_count[i] = res.size();
    if (res.size() < 2U) {
      continue;
    }
    const double n = static_cast<double>(res.size());
    double sum = 0.0;
    for (const double val : res) sum += val;
    const double mean = sum / n;
    diag.residual_mean[i] = mean;

    double sq_diff = 0.0;
    for (const double val : res) {
      const double d = val - mean;
      sq_diff += d * d;
    }
    diag.residual_variance[i] = sq_diff / (n - 1.0);

    // KS test against Exp(1): CDF = 1 - exp(-x)
    std::vector<double> sorted_res = res;
    std::sort(sorted_res.begin(), sorted_res.end());
    double max_ks_plus = 0.0;
    double max_ks_minus = 0.0;
    for (std::size_t k = 0; k < sorted_res.size(); ++k) {
      const double theo_cdf = 1.0 - std::exp(-sorted_res[k]);
      const double empirical_upper = static_cast<double>(k + 1U) / n;
      const double empirical_lower = static_cast<double>(k) / n;
      max_ks_plus = std::max(max_ks_plus, empirical_upper - theo_cdf);
      max_ks_minus = std::max(max_ks_minus, theo_cdf - empirical_lower);
    }
    diag.ks_statistic[i] = std::max(max_ks_plus, max_ks_minus);
  }
}

// ---------------------------------------------------------------------------
// Maximum-Likelihood Estimation Pipeline
// ---------------------------------------------------------------------------

EstimationResult HawkesEstimator::fit(
    const EstimationDataset& dataset,
    const OptimizationSettings& settings) {
  validate_dataset(dataset);
  if (settings.structure != ModelStructure::IndependentPoisson &&
      (settings.num_restarts == 0U || settings.max_iterations == 0U ||
       !std::isfinite(settings.tolerance) || settings.tolerance <= 0.0 ||
       !std::isfinite(settings.max_spectral_radius) ||
       settings.max_spectral_radius >= 1.0 ||
       settings.max_spectral_radius <= 0.01 ||
       (settings.fixed_decay_per_second.has_value() &&
        (!std::isfinite(*settings.fixed_decay_per_second) ||
         *settings.fixed_decay_per_second <= 0.0)))) {
    throw std::invalid_argument{"invalid Hawkes optimization settings"};
  }
  const auto dim = dataset.dimension;
  const double T = dataset.horizon_seconds;

  switch (settings.structure) {
    case ModelStructure::SharedParentDecay:
      throw std::invalid_argument{
          "parent-specific shared decay fitting is not implemented"};
    case ModelStructure::Bivariate2D:
      if (dim != 2U) {
        throw std::invalid_argument{"bivariate fitting requires dimension 2"};
      }
      break;
    case ModelStructure::Univariate1D:
      if (dim != 1U) {
        throw std::invalid_argument{"univariate fitting requires dimension 1"};
      }
      break;
    case ModelStructure::FullMultivariate:
    case ModelStructure::GlobalDecay:
    case ModelStructure::DiagonalSelfExcitation:
      break;
    case ModelStructure::IndependentPoisson:
      return fit_poisson(dataset);
    default:
      throw std::invalid_argument{"unknown Hawkes model structure"};
  }

  // Baseline independent Poisson benchmark for LRT and AIC/BIC
  const auto poisson_result = fit_poisson(dataset);

  // Initialize candidate parameters
  stochastic::HawkesParameters best_params = poisson_result.parameters;
  double best_ll = -std::numeric_limits<double>::infinity();
  std::size_t total_evals = 0;
  bool best_candidate_converged = false;
  std::size_t best_candidate_iterations = 0;

  // Determine candidate decay rates to profile/evaluate
  std::vector<double> beta_candidates;
  if (settings.fixed_decay_per_second.has_value()) {
    beta_candidates.push_back(*settings.fixed_decay_per_second);
  } else {
    // Multi-scale candidate decay rates (e.g. 1.0, 5.0, 10.0, 20.0)
    beta_candidates = {1.0, 5.0, 10.0};
  }

  std::mt19937_64 rng(settings.seed);
  std::uniform_real_distribution<double> alpha_init_dist(0.01, 0.10);

  // For each candidate decay:
  for (const double candidate_beta : beta_candidates) {
    for (std::size_t start = 0; start < settings.num_restarts; ++start) {
      stochastic::HawkesParameters cur_params;
      cur_params.baseline_events_per_second = poisson_result.parameters.baseline_events_per_second;
      // Slight shrink of initial baselines to leave room for excitation
      for (double& mu : cur_params.baseline_events_per_second) {
        mu = std::max(1.0e-4, mu * 0.7);
      }
      cur_params.alpha_branching.assign(dim, std::vector<double>(dim, 0.0));
      cur_params.beta_per_second.assign(dim, std::vector<double>(dim, candidate_beta));

      // Random small initial alpha
      for (std::size_t i = 0; i < dim; ++i) {
        if (settings.structure == ModelStructure::DiagonalSelfExcitation) {
          cur_params.alpha_branching[i][i] = alpha_init_dist(rng);
        } else {
          for (std::size_t j = 0; j < dim; ++j) {
            cur_params.alpha_branching[i][j] = alpha_init_dist(rng) / static_cast<double>(dim);
          }
        }
      }

      // Projected gradient ascent with a deterministic decaying step size.
      bool converged = false;
      std::size_t iter = 0;

      for (; iter < settings.max_iterations; ++iter) {
        ++total_evals;
        const auto grads = compute_mu_alpha_gradients(dataset, cur_params);

        double max_grad_norm = 0.0;
        for (std::size_t i = 0; i < dim; ++i) {
          for (const double g : grads[i]) {
            max_grad_norm = std::max(max_grad_norm, std::abs(g));
          }
        }

        if (max_grad_norm < settings.tolerance) {
          converged = true;
          break;
        }

        // Adaptive step size
        double step_size = 0.05 / (1.0 + 0.005 * static_cast<double>(iter));

        // Update parameters
        for (std::size_t i = 0; i < dim; ++i) {
          // mu update
          double new_mu = cur_params.baseline_events_per_second[i] + step_size * grads[i][0] / T;
          cur_params.baseline_events_per_second[i] = std::max(1.0e-5, new_mu);

          // alpha update
          for (std::size_t j = 0; j < dim; ++j) {
            if (settings.structure == ModelStructure::DiagonalSelfExcitation && i != j) {
              cur_params.alpha_branching[i][j] = 0.0;
              continue;
            }
            double new_alpha = cur_params.alpha_branching[i][j] + step_size * grads[i][1 + j] / T;
            // Enforce nonnegativity constraint
            cur_params.alpha_branching[i][j] = std::max(0.0, new_alpha);
          }
        }

        // Check spectral radius stability constraint
        const double rho = stochastic::branching_spectral_radius(cur_params);
        if (rho >= settings.max_spectral_radius && rho > 0.0) {
          const double scale = (settings.max_spectral_radius - 0.01) / rho;
          for (std::size_t i = 0; i < dim; ++i) {
            for (std::size_t j = 0; j < dim; ++j) {
              cur_params.alpha_branching[i][j] *= scale;
            }
          }
        }
      }

      const double ll = log_likelihood(dataset, cur_params);
      if (ll > best_ll) {
        best_ll = ll;
        best_params = cur_params;
        best_candidate_converged = converged;
        best_candidate_iterations = iter < settings.max_iterations
                                        ? iter + 1U
                                        : settings.max_iterations;
      }
    }
  }

  // Construct final result
  EstimationResult result;
  result.dataset_name = dataset.dataset_name;
  result.horizon_seconds = dataset.horizon_seconds;
  result.total_events = dataset.total_events();
  result.parameters = best_params;
  result.settings = settings;

  // Diagnostics
  result.diagnostics.converged = best_candidate_converged;
  result.diagnostics.iterations_used = best_candidate_iterations;
  result.diagnostics.function_evaluations = total_evals;
  result.diagnostics.final_log_likelihood = best_ll;
  result.diagnostics.poisson_log_likelihood = poisson_result.diagnostics.final_log_likelihood;

  // Likelihood ratio test
  const double lrt = 2.0 * (best_ll - poisson_result.diagnostics.final_log_likelihood);
  result.diagnostics.lrt_statistic = std::max(0.0, lrt);

  // Degrees of freedom: number of non-zero alpha parameters estimated
  std::size_t df = 0;
  for (std::size_t i = 0; i < dim; ++i) {
    for (std::size_t j = 0; j < dim; ++j) {
      if (settings.structure != ModelStructure::DiagonalSelfExcitation || i == j) {
        ++df;
      }
    }
  }
  result.diagnostics.degrees_of_freedom = df;
  result.diagnostics.lrt_p_value = chi2_p_value(result.diagnostics.lrt_statistic, static_cast<double>(df));

  // AIC & BIC
  const double estimated_decay_parameters =
      settings.fixed_decay_per_second.has_value() ? 0.0 : 1.0;
  const double num_params =
      static_cast<double>(dim + df) + estimated_decay_parameters;
  const double n_events = static_cast<double>(std::max<std::size_t>(1, dataset.total_events()));
  result.diagnostics.aic = 2.0 * num_params - 2.0 * best_ll;
  result.diagnostics.bic = num_params * std::log(n_events) - 2.0 * best_ll;
  const auto final_gradients =
      compute_mu_alpha_gradients(dataset, best_params);
  for (const auto& row : final_gradients) {
    for (const double value : row) {
      result.diagnostics.gradient_norm =
          std::max(result.diagnostics.gradient_norm, std::abs(value));
    }
  }

  // Stability & stationary mean
  result.diagnostics.spectral_radius = stochastic::branching_spectral_radius(best_params);
  result.diagnostics.is_subcritical = (result.diagnostics.spectral_radius < 1.0);
  if (result.diagnostics.is_subcritical) {
    result.diagnostics.stationary_mean = stochastic::stationary_mean_intensity(best_params);
  }

  // Standard errors from Fisher information
  result.parameter_standard_errors.baseline_events_per_second.assign(dim, 0.0);
  result.parameter_standard_errors.alpha_branching.assign(dim, std::vector<double>(dim, 0.0));
  result.parameter_standard_errors.beta_per_second.assign(dim, std::vector<double>(dim, 0.0));

  for (std::size_t i = 0; i < dim; ++i) {
    const auto [ses, identified] = compute_standard_errors_for_row(dataset, best_params, i);
    if (!identified) {
      result.diagnostics.weak_identification_detected = true;
    }
    if (ses.size() >= 1 + dim) {
      result.parameter_standard_errors.baseline_events_per_second[i] = ses[0];
      for (std::size_t j = 0; j < dim; ++j) {
        result.parameter_standard_errors.alpha_branching[i][j] = ses[1 + j];
      }
    }
  }

  if (result.diagnostics.weak_identification_detected) {
    result.diagnostics.warnings.push_back(
        "Weak parameter identification detected on one or more channels due to sparse events or collinearity.");
  }

  // Goodness-of-fit residuals
  evaluate_goodness_of_fit(dataset, best_params, result.diagnostics);

  return result;
}

// ---------------------------------------------------------------------------
// Dataset Construction Helpers
// ---------------------------------------------------------------------------

EstimationDataset HawkesEstimator::from_historical_events(
    const std::vector<historical::MappedHistoricalEvent>& events,
    double horizon_seconds,
    const std::string& name) {
  EstimationDataset ds;
  ds.dataset_name = name;
  ds.provenance = "historical_ingestion";
  ds.dimension = stochastic::kEventTypeCount;
  ds.event_counts_by_type.assign(ds.dimension, 0);

  ds.type_labels.reserve(ds.dimension);
  for (std::size_t i = 0; i < ds.dimension; ++i) {
    ds.type_labels.push_back(std::string(stochastic::kEventTypeLabels[i]));
  }

  if (events.empty()) {
    ds.horizon_seconds = horizon_seconds > 0.0 ? horizon_seconds : 1.0;
    return ds;
  }

  const core::TimestampNs origin = events.front().timestamp_ns;
  ds.observations.reserve(events.size());

  core::TimestampNs max_ts = events.front().timestamp_ns;

  for (const auto& ev : events) {
    max_ts = std::max(max_ts, ev.timestamp_ns);
    const double t_s = static_cast<double>(ev.timestamp_ns - origin) / 1.0e9;
    const auto idx = stochastic::event_type_index(ev.hawkes_type);
    ds.observations.push_back({.time_seconds = std::max(0.0, t_s), .type_index = idx});
    if (idx < ds.event_counts_by_type.size()) {
      ++ds.event_counts_by_type[idx];
    }
  }

  if (horizon_seconds > 0.0) {
    ds.horizon_seconds = horizon_seconds;
  } else {
    ds.horizon_seconds = std::max(1.0, static_cast<double>(max_ts - origin) / 1.0e9);
  }

  return ds;
}

EstimationDataset HawkesEstimator::from_synthetic_events(
    const std::vector<stochastic::MarketEventRecord>& events,
    const double horizon_seconds,
    const std::string& name) {
  EstimationDataset ds;
  ds.dataset_name = name;
  ds.provenance = "synthetic_simulation";
  ds.dimension = stochastic::kEventTypeCount;
  ds.horizon_seconds = horizon_seconds > 0.0 ? horizon_seconds : 1.0;
  ds.event_counts_by_type.assign(ds.dimension, 0);

  ds.type_labels.reserve(ds.dimension);
  for (std::size_t i = 0; i < ds.dimension; ++i) {
    ds.type_labels.push_back(std::string(stochastic::kEventTypeLabels[i]));
  }

  ds.observations.reserve(events.size());
  for (const auto& ev : events) {
    const auto idx = stochastic::event_type_index(ev.type);
    ds.observations.push_back({.time_seconds = ev.time_seconds, .type_index = idx});
    if (idx < ds.event_counts_by_type.size()) {
      ++ds.event_counts_by_type[idx];
    }
  }

  return ds;
}

// ---------------------------------------------------------------------------
// Reporting & JSON
// ---------------------------------------------------------------------------

std::string EstimationResult::to_formatted_report() const {
  std::ostringstream out;
  out << std::fixed << std::setprecision(4);

  out << "================================================================================\n"
      << "                      HAWKES PROCESS CALIBRATION REPORT                         \n"
      << "================================================================================\n"
      << "Dataset:                 " << dataset_name << "\n"
      << "Observation Window:      [0.0000, " << horizon_seconds << "] seconds\n"
      << "Total Event Count:       " << total_events << "\n"
      << "Model Structure:         ";
  switch (settings.structure) {
    case ModelStructure::FullMultivariate: out << "Full Multivariate"; break;
    case ModelStructure::SharedParentDecay: out << "Shared Parent Decay"; break;
    case ModelStructure::GlobalDecay: out << "Global Decay"; break;
    case ModelStructure::DiagonalSelfExcitation: out << "Diagonal Self-Excitation"; break;
    case ModelStructure::Bivariate2D: out << "Bivariate (2D)"; break;
    case ModelStructure::Univariate1D: out << "Univariate (1D)"; break;
    case ModelStructure::IndependentPoisson: out << "Independent Poisson"; break;
  }
  out << "\n\n"
      << "1. CONVERGENCE & MODEL COMPARISON\n"
      << "--------------------------------------------------------------------------------\n"
      << "  Convergence Status:    " << (diagnostics.converged ? "CONVERGED" : "MAX_ITERATIONS_REACHED") << "\n"
      << "  Iterations Used:       " << diagnostics.iterations_used << "\n"
      << "  Log-Likelihood (Hawkes):  " << diagnostics.final_log_likelihood << "\n"
      << "  Log-Likelihood (Poisson): " << diagnostics.poisson_log_likelihood << "\n"
      << "  Likelihood Ratio Stat:    " << diagnostics.lrt_statistic << " (df=" << diagnostics.degrees_of_freedom
      << ", p-value=" << diagnostics.lrt_p_value << ")\n"
      << "  AIC (Hawkes vs Poisson):  " << diagnostics.aic << " vs "
      << (2.0 * static_cast<double>(parameters.baseline_events_per_second.size()) - 2.0 * diagnostics.poisson_log_likelihood) << "\n"
      << "  BIC:                      " << diagnostics.bic << "\n\n"
      << "2. STABILITY DIAGNOSTICS\n"
      << "--------------------------------------------------------------------------------\n"
      << "  Spectral Radius rho(A):   " << diagnostics.spectral_radius << "\n"
      << "  Stationary Stability:     " << (diagnostics.is_subcritical ? "SUBCRITICAL (STABLE)" : "SUPERCRITICAL (UNSTABLE)") << "\n";

  if (diagnostics.is_subcritical && !diagnostics.stationary_mean.empty()) {
    out << "  Stationary Mean Rate (events/s): [";
    for (std::size_t i = 0; i < diagnostics.stationary_mean.size(); ++i) {
      if (i > 0) out << ", ";
      out << diagnostics.stationary_mean[i];
    }
    out << "]\n\n";
  }

  out << "3. PARAMETER ESTIMATES (Estimate +/- SE)\n"
      << "--------------------------------------------------------------------------------\n";
  for (std::size_t i = 0; i < parameters.baseline_events_per_second.size(); ++i) {
    const std::string label = (i < stochastic::kEventTypeCount) ? std::string(stochastic::kEventTypeLabels[i]) : std::to_string(i);
    out << "  Channel " << label << " Baseline mu: "
        << parameters.baseline_events_per_second[i] << " +/- "
        << parameter_standard_errors.baseline_events_per_second[i] << " /s\n";
  }
  out << "\n  Branching Matrix Alpha (row=child, col=parent):\n";
  for (std::size_t i = 0; i < parameters.alpha_branching.size(); ++i) {
    const std::string label = (i < stochastic::kEventTypeCount) ? std::string(stochastic::kEventTypeLabels[i]) : std::to_string(i);
    out << "    " << label << ": [";
    for (std::size_t j = 0; j < parameters.alpha_branching[i].size(); ++j) {
      if (j > 0) out << ", ";
      out << parameters.alpha_branching[i][j];
    }
    out << "]\n";
  }

  out << "\n4. GOODNESS-OF-FIT (TIME-CHANGE THEOREM RESIDUALS)\n"
      << "--------------------------------------------------------------------------------\n"
      << "  (Target for correctly specified model: Mean = 1.0, Variance = 1.0)\n";
  for (std::size_t i = 0; i < diagnostics.residual_mean.size(); ++i) {
    const std::string label = (i < stochastic::kEventTypeCount) ? std::string(stochastic::kEventTypeLabels[i]) : std::to_string(i);
    const std::size_t count =
        i < diagnostics.residual_count.size() ? diagnostics.residual_count[i] : 0U;
    out << "    Channel " << label << ": n=" << count;
    if (count < 2U) {
      out << " (not evaluated: fewer than two complete residuals)\n";
      continue;
    }
    out << ", Residual Mean=" << diagnostics.residual_mean[i]
        << ", Var=" << diagnostics.residual_variance[i]
        << ", KS-Stat=" << diagnostics.ks_statistic[i] << "\n";
  }

  if (!diagnostics.warnings.empty()) {
    out << "\n5. IDENTIFICATION & DATA WARNINGS\n"
        << "--------------------------------------------------------------------------------\n";
    for (const auto& w : diagnostics.warnings) {
      out << "  [WARNING] " << w << "\n";
    }
  }

  out << "\nLIMITATIONS & PROVENANCE NOTICE:\n"
      << "  This calibration is conducted under documented synthetic/reconstructed test\n"
      << "  conditions. Parameters from finite samples must not be claimed as true empirical\n"
      << "  market constants without validated historical venue L1/L2 feeds.\n"
      << "================================================================================\n";

  return out.str();
}

std::string EstimationResult::to_json() const {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6);
  out << "{\n"
      << "  \"dataset_name\": \"" << dataset_name << "\",\n"
      << "  \"horizon_seconds\": " << horizon_seconds << ",\n"
      << "  \"total_events\": " << total_events << ",\n"
      << "  \"converged\": " << (diagnostics.converged ? "true" : "false") << ",\n"
      << "  \"log_likelihood\": " << diagnostics.final_log_likelihood << ",\n"
      << "  \"poisson_log_likelihood\": " << diagnostics.poisson_log_likelihood << ",\n"
      << "  \"lrt_statistic\": " << diagnostics.lrt_statistic << ",\n"
      << "  \"lrt_p_value\": " << diagnostics.lrt_p_value << ",\n"
      << "  \"aic\": " << diagnostics.aic << ",\n"
      << "  \"bic\": " << diagnostics.bic << ",\n"
      << "  \"spectral_radius\": " << diagnostics.spectral_radius << ",\n"
      << "  \"is_subcritical\": " << (diagnostics.is_subcritical ? "true" : "false") << ",\n"
      << "  \"baselines\": [";
  for (std::size_t i = 0; i < parameters.baseline_events_per_second.size(); ++i) {
    if (i > 0) out << ",";
    out << parameters.baseline_events_per_second[i];
  }
  out << "],\n"
      << "  \"alphas\": [";
  for (std::size_t i = 0; i < parameters.alpha_branching.size(); ++i) {
    if (i > 0) out << ",";
    out << "[";
    for (std::size_t j = 0; j < parameters.alpha_branching[i].size(); ++j) {
      if (j > 0) out << ",";
      out << parameters.alpha_branching[i][j];
    }
    out << "]";
  }
  out << "],\n"
      << "  \"time_change_residuals\": [";
  for (std::size_t i = 0; i < diagnostics.residual_count.size(); ++i) {
    if (i > 0) out << ",";
    const std::size_t count = diagnostics.residual_count[i];
    out << "{\"count\":" << count << ",\"mean\":";
    if (count < 2U) out << "null";
    else out << diagnostics.residual_mean[i];
    out << ",\"variance\":";
    if (count < 2U) out << "null";
    else out << diagnostics.residual_variance[i];
    out << ",\"ks_statistic\":";
    if (count < 2U) out << "null";
    else out << diagnostics.ks_statistic[i];
    out << "}";
  }
  out << "]\n}\n";
  return out.str();
}

}  // namespace lob::estimation
