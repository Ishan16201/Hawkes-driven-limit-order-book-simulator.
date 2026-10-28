#include "lob/stochastic/hawkes_parameters.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace lob::stochastic {
namespace {

[[nodiscard]] bool is_finite_positive(const double value) noexcept {
  return std::isfinite(value) && value > 0.0;
}

}  // namespace

double branching_upper_bound(const HawkesParameters& parameters) {
  double maximum = 0.0;
  for (const auto& row : parameters.alpha_branching) {
    double row_sum = 0.0;
    for (const double value : row) {
      row_sum += std::abs(value);
    }
    maximum = std::max(maximum, row_sum);
  }
  return maximum;
}

double branching_spectral_radius(const HawkesParameters& parameters) {
  const std::size_t n = parameters.alpha_branching.size();
  if (n == 0U) {
    return 0.0;
  }
  for (const auto& row : parameters.alpha_branching) {
    if (row.size() != n) {
      throw std::invalid_argument{"branching matrix must be square"};
    }
    for (const double value : row) {
      if (!std::isfinite(value) || value < 0.0) {
        throw std::invalid_argument{
            "branching coefficients must be finite and nonnegative"};
      }
    }
  }

  // Shifting by I removes the periodic oscillation that makes ordinary power
  // iteration return alternating norm estimates for matrices such as
  // [[0, a], [b, 0]]. The Perron root of A + I is rho(A) + 1.
  std::vector<double> vector(n, 1.0 / static_cast<double>(n));
  double estimate = 0.0;
  for (std::size_t iteration = 0; iteration < 10000U; ++iteration) {
    std::vector<double> next(n, 0.0);
    for (std::size_t row = 0; row < n; ++row) {
      next[row] = vector[row];
      for (std::size_t column = 0; column < n; ++column) {
        next[row] += parameters.alpha_branching[row][column] * vector[column];
      }
    }

    double norm = 0.0;
    for (const double value : next) {
      norm += value;
    }
    if (!std::isfinite(norm)) {
      throw std::overflow_error{"branching spectral-radius iteration overflow"};
    }
    estimate = norm - 1.0;
    for (double& value : next) {
      value /= norm;
    }

    double difference = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      difference += std::abs(next[i] - vector[i]);
    }
    vector = std::move(next);
    if (difference < 1.0e-14) {
      break;
    }
  }
  return std::max(0.0, estimate);
}

std::vector<double> stationary_mean_intensity(const HawkesParameters& p) {
  validate_stationary(p);
  const std::size_t n = p.baseline_events_per_second.size();
  std::vector<std::vector<double>> matrix(
      n, std::vector<double>(n + 1U, 0.0));
  for (std::size_t row = 0; row < n; ++row) {
    for (std::size_t column = 0; column < n; ++column) {
      matrix[row][column] =
          (row == column ? 1.0 : 0.0) - p.alpha_branching[row][column];
    }
    matrix[row][n] = p.baseline_events_per_second[row];
  }

  for (std::size_t column = 0; column < n; ++column) {
    std::size_t pivot = column;
    for (std::size_t row = column + 1U; row < n; ++row) {
      if (std::abs(matrix[row][column]) >
          std::abs(matrix[pivot][column])) {
        pivot = row;
      }
    }
    if (matrix[pivot][column] == 0.0 ||
        !std::isfinite(matrix[pivot][column])) {
      throw std::overflow_error{"singular stationary mean system"};
    }
    std::swap(matrix[pivot], matrix[column]);
    const double scale = matrix[column][column];
    for (std::size_t j = column; j <= n; ++j) {
      matrix[column][j] /= scale;
    }
    for (std::size_t row = 0; row < n; ++row) {
      if (row == column) {
        continue;
      }
      const double factor = matrix[row][column];
      for (std::size_t j = column; j <= n; ++j) {
        matrix[row][j] -= factor * matrix[column][j];
      }
    }
  }

  std::vector<double> result(n);
  for (std::size_t i = 0; i < n; ++i) {
    result[i] = matrix[i][n];
    if (!std::isfinite(result[i]) || result[i] < 0.0) {
      throw std::overflow_error{"stationary mean intensity is not representable"};
    }
  }
  return result;
}

void validate_stationary(const HawkesParameters& parameters) {
  const std::size_t dimension = parameters.baseline_events_per_second.size();
  if (dimension == 0U || parameters.alpha_branching.size() != dimension ||
      parameters.beta_per_second.size() != dimension) {
    throw std::invalid_argument{"Hawkes parameter dimensions must be nonzero and consistent"};
  }

  for (const double baseline : parameters.baseline_events_per_second) {
    if (!is_finite_positive(baseline)) {
      throw std::invalid_argument{"baseline intensities must be finite and positive"};
    }
  }
  for (std::size_t row = 0U; row < dimension; ++row) {
    if (parameters.alpha_branching[row].size() != dimension ||
        parameters.beta_per_second[row].size() != dimension) {
      throw std::invalid_argument{"Hawkes matrices must be square"};
    }
    for (std::size_t column = 0U; column < dimension; ++column) {
      if (!std::isfinite(parameters.alpha_branching[row][column]) || parameters.alpha_branching[row][column] < 0.0) {
        throw std::invalid_argument{"branching coefficients must be finite and nonnegative"};
      }
      if (!is_finite_positive(parameters.beta_per_second[row][column])) {
        throw std::invalid_argument{"decay rates must be finite and positive"};
      }
    }
  }
  if (branching_spectral_radius(parameters) >= 1.0) {
    throw std::invalid_argument{"integrated branching spectral radius must be below one"};
  }
}

}  // namespace lob::stochastic
