# Hawkes calibration implementation

## Likelihood and units

For event observations `(t_k, m_k)` on `[0,T]`, the current estimator uses

```text
lambda_i(t) = mu_i + sum_j sum_{k:m_k=j, t_k<t}
              alpha_ij * beta_ij * exp(-beta_ij*(t-t_k))

log L = sum_k log(lambda_{m_k}(t_k)) - sum_i Lambda_i(T)
Lambda_i(T) = mu_i*T + sum_j alpha_ij *
              sum_{k:m_k=j}(1-exp(-beta_ij*(T-t_k))).
```

Rows are child types and columns are parent types. `mu` has units events/second,
`alpha` is dimensionless integrated branching mass, and `beta` is inverse
seconds. Empty histories retain their baseline compensator, so
`log L = -T * sum(mu_i)` for an empty observation set.

The recursive likelihood state decays exactly between observations. Analytic
gradients with respect to each baseline and branching coefficient are tested
against central finite differences. Input observations must be finite,
nondecreasing, within the finite positive horizon, and use valid type indexes.
An explicitly supplied count vector must match observations.

## What the fitter currently does

`HawkesEstimator::fit` compares a restricted Hawkes fit to an independent
Poisson baseline. The estimator is an in-process C++ API; no calibration
command-line executable or JSON config ingestion exists.

- `IndependentPoisson` fits a closed-form baseline by type. A zero-count
  channel uses a numerical rate floor of `1e-8` events/second, and its
  compensator is included in likelihood.
- `DiagonalSelfExcitation` constrains off-diagonal alpha entries to zero.
- `FullMultivariate`, `GlobalDecay`, `Bivariate2D`, and `Univariate1D` fit a
  full alpha matrix (with the specified dimension check for the last two).
- Decay is either fixed by `fixed_decay_per_second` or selected from the
  discrete common-decay profile `{1, 5, 10}` inverse seconds. It is not a
  continuous beta optimizer. `SharedParentDecay` is reserved and now fails
  explicitly rather than silently acting like a global decay model.
- Baselines/alphas use deterministic seeded starts and projected gradient
  ascent with a decaying step size. The code does not implement an Armijo line
  search. Stability correction scales alpha when the spectral radius reaches
  `max_spectral_radius`; the safety margin is 0.01 and the setting must exceed
  0.01.
- Convergence is based on the maximum absolute, unscaled log-likelihood
  gradient. The reported convergence status/iteration count refer to the
  best-likelihood start; total function evaluations cover all starts.
- When beta is profiled, AIC/BIC count one selected common-decay parameter.
  Fixed beta is not counted as estimated.

Consequently, a result should be described as a constrained coarse-profile fit,
not an unconstrained full-matrix MLE over all `alpha_ij` and `beta_ij`. The
returned standard errors are based on the inverse empirical outer-product
Fisher matrix for each row and are asymptotic diagnostics, not calibrated
finite-sample confidence intervals.

## Goodness-of-fit and model comparison

The diagnostic applies the per-type compensator time change and records
successive complete residuals. It reports count, sample mean, unbiased sample
variance, and the two-sided one-sample KS statistic against `Exp(1)`. It does
not report a KS p-value. Channels with fewer than two residuals have metrics
marked unavailable (`null` in JSON); they are not assigned ideal values.
Residuals after the final observed event are right-censored at the end of the
window and are not included in the sample summaries.

The reported likelihood-ratio p-value uses a Wilson-Hilferty approximation to
the chi-square upper tail. Since excitation coefficients are constrained to be
nonnegative and the decay can be selected from a finite profile grid, the
usual regular chi-square reference may not apply. Treat this value as a rough
diagnostic, not formal evidence. A failure to reject a goodness-of-fit null
hypothesis does not establish that the model is correct.

## Verification actually present

Automated tests cover finite-difference gradients, the Poisson formula,
synthetic fitting against a Poisson baseline, compensator residual calculations,
two-sided KS behavior, sparse-channel handling, and empty-history likelihood.
A limited five-seed univariate experiment with fixed known beta checks baseline
and alpha recovery against three across-seed standard errors and compares the
true beta likelihood to one misspecified beta. This is not a broad recovery,
power, or misspecification study.

There is no historical venue dataset or out-of-sample calibration run in the
repository. Do not claim empirical calibration. The API can be exercised from
C++ tests; the reproducible synthetic CLI demonstrates simulation and analytics
only, not calibration.
