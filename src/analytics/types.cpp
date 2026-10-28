#include "lob/analytics/types.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace lob::analytics {

SampleStats compute_sample_stats(
    const std::vector<double>& values, std::size_t hac_max_lags) {
  SampleStats stats;
  stats.count = values.size();

  if (values.empty()) {
    return stats;
  }

  if (values.size() == 1U) {
    stats.mean = values[0];
    stats.min = values[0];
    stats.max = values[0];
    stats.median = values[0];
    stats.p25 = values[0];
    stats.p75 = values[0];
    stats.p95 = values[0];
    stats.p99 = values[0];
    stats.effective_sample_size = 1.0;
    return stats;
  }

  const double n_double = static_cast<double>(values.size());

  // Mean
  const double sum = std::accumulate(values.begin(), values.end(), 0.0);
  stats.mean = sum / n_double;

  // Sample variance and autocovariances
  double sum_sq_diff = 0.0;
  for (const double val : values) {
    const double diff = val - stats.mean;
    sum_sq_diff += diff * diff;
  }

  // Sample variance s^2 with Bessel's correction (N-1)
  const double s2 = sum_sq_diff / (n_double - 1.0);
  stats.std_dev = std::sqrt(std::max(0.0, s2));

  // Population variance gamma_0 with N denominator for autocovariances
  const double gamma_0 = sum_sq_diff / n_double;

  // Percentiles
  std::vector<double> sorted = values;
  std::sort(sorted.begin(), sorted.end());

  stats.min = sorted.front();
  stats.max = sorted.back();

  auto compute_quantile = [&sorted, n_double](const double p) -> double {
    if (sorted.empty()) return 0.0;
    if (sorted.size() == 1U) return sorted[0];
    const double h = (n_double - 1.0) * p;
    const auto i = static_cast<std::size_t>(std::floor(h));
    const double frac = h - static_cast<double>(i);
    if (i + 1U >= sorted.size()) {
      return sorted.back();
    }
    return (1.0 - frac) * sorted[i] + frac * sorted[i + 1U];
  };

  stats.p25 = compute_quantile(0.25);
  stats.median = compute_quantile(0.50);
  stats.p75 = compute_quantile(0.75);
  stats.p95 = compute_quantile(0.95);
  stats.p99 = compute_quantile(0.99);

  // i.i.d. Standard Error
  stats.se_iid = stats.std_dev / std::sqrt(n_double);

  // Newey-West HAC Standard Error
  if (hac_max_lags == 0) {
    // Default rule of thumb: 4 * (N / 100)^(2/9)
    const double auto_lags = 4.0 * std::pow(n_double / 100.0, 2.0 / 9.0);
    hac_max_lags = std::min<std::size_t>(20U, static_cast<std::size_t>(std::max(1.0, std::floor(auto_lags))));
  }
  const std::size_t max_l = std::min(hac_max_lags, values.size() - 1U);

  double hac_sum = gamma_0;
  for (std::size_t k = 1; k <= max_l; ++k) {
    double autocov_k = 0.0;
    for (std::size_t i = k; i < values.size(); ++i) {
      autocov_k += (values[i] - stats.mean) * (values[i - k] - stats.mean);
    }
    autocov_k /= n_double;

    if (k == 1U && gamma_0 > 1.0e-12) {
      stats.autocorr_lag1 = autocov_k / gamma_0;
    }

    const double bartlett_weight = 1.0 - static_cast<double>(k) / static_cast<double>(max_l + 1U);
    hac_sum += 2.0 * bartlett_weight * autocov_k;
  }

  // Asymptotic variance of sample mean is hac_sum / N
  double var_mean = hac_sum / n_double;
  if (var_mean < 0.0) {
    var_mean = s2 / n_double;
  }
  stats.se_hac = std::sqrt(std::max(0.0, var_mean));

  // Effective sample size
  if (std::abs(stats.autocorr_lag1) < 0.9999) {
    const double ess = n_double * (1.0 - stats.autocorr_lag1) / (1.0 + stats.autocorr_lag1);
    stats.effective_sample_size = std::max(1.0, std::min(ess, n_double * 10.0));
  } else {
    stats.effective_sample_size = 1.0;
  }

  return stats;
}

}  // namespace lob::analytics
