# Multivariate exponential Hawkes process

Rows of `alpha[i][j]` and `beta[i][j]` are child intensity types `i`; columns
are parent event types `j`. The kernel is
`alpha[i][j] * beta[i][j] * exp(-beta[i][j] u)`, so its integral is exactly
`alpha[i][j]`. Thus `A=alpha` is the branching matrix, `rho(A)<1` is required,
and the stationary mean is `(I-A)^-1 mu`. `beta` has inverse-time units and is
not part of the stability matrix.

The process stores post-history states `x[i][j]`. It decays them exactly between
times and evaluates intensities immediately before a candidate/event. Only after
an event of type `k` is accepted does it add `alpha[i][k]*beta[i][k]` to every
child row, preventing self-excitation from influencing its own arrival test.

Ogata thinning uses the current total intensity as an upper bound because all
nonnegative exponential contributions decay between events. Rejected candidates
advance and retain the decayed state. Finite simulations have burn-in and sample
variability; mean-intensity checks therefore use long runs and broad sampling
uncertainty, not exact sample equality.
