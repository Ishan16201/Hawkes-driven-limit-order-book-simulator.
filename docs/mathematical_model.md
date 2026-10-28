# Mathematical model and simulation choices

## Multivariate exponential Hawkes process

There are `M` event types with counting processes `N_j(t)`. The conditional
intensity of child type `i` is

```text
lambda_i(t) = mu_i
            + sum_j sum_{k: m_k=j, t_k<t}
                alpha_ij * beta_ij * exp(-beta_ij * (t - t_k)).
```

Rows of `alpha` and `beta` are child types; columns are parent types. The
parameter units are:

| Parameter | Meaning | Unit / constraint |
|---|---|---|
| `t`, `T` | Statistical event time and horizon | seconds |
| `lambda_i`, `mu_i` | Conditional/baseline rate | events per second; baseline finite and strictly positive |
| `alpha_ij` | Integrated kernel / expected direct children | dimensionless, finite, nonnegative |
| `beta_ij` | Exponential decay rate | inverse seconds, finite and strictly positive |
| `phi_ij(u)` | Kernel contribution | `alpha * beta * exp(-beta*u)` |

The kernel integrates to `alpha_ij`; therefore the integrated branching matrix
is `A = [alpha_ij]`, not `alpha*beta`. The stationary subcritical condition is
`rho(A) < 1`. When it holds, the stationary mean is
`bar_lambda = (I - A)^(-1) mu`. A zero baseline is not accepted by the current
process validator.

The configured six-type taxonomy is:

| Index | Label | Interpretation |
|---:|---|---|
| 0 | LB | Limit buy submission |
| 1 | LA | Limit sell submission |
| 2 | CB | Buy-side cancellation |
| 3 | CA | Sell-side cancellation |
| 4 | MB | Aggressive buy / market buy |
| 5 | MS | Aggressive sell / market sell |

This mapping and the event-to-order mark rules are modeling assumptions, not
empirical findings.

## Recursive state and likelihood

For each child-parent pair, the process stores the current excitation
contribution

```text
x_ij(t) = sum_{k: m_k=j, t_k<t} alpha_ij * beta_ij
          * exp(-beta_ij * (t - t_k)).
```

Between times, `x_ij` is multiplied by `exp(-beta_ij * dt)`. An accepted event
of parent type `j` adds `alpha_ij * beta_ij` to each child row at the event
time. Intensities logged with an event are pre-jump intensities.

The conditional log likelihood on `[0,T]` is

```text
sum_k log(lambda_{m_k}(t_k)) - sum_i Lambda_i(T)
Lambda_i(T) = mu_i*T
            + sum_j alpha_ij * sum_{k:m_k=j}
                (1 - exp(-beta_ij*(T-t_k))).
```

An empty event history has log likelihood `-T * sum_i mu_i`; it is not assigned
a zero likelihood by convention.

## Event-time generation

`ExponentialHawkesProcess` uses Ogata thinning. With nonnegative exponential
kernels, the current total intensity is a valid proposal-rate upper bound
because all excitation terms decay between accepted events. The process:

1. Samples an exponential proposal wait from the current total intensity.
2. Advances all excitation states exactly to the proposed time.
3. Accepts with probability `current_total / proposal_total`.
4. Chooses an event type proportional to the pre-event channel intensities.
5. Applies the corresponding excitation jump only after acceptance.

Rejected proposals still advance and decay the state. The implementation uses
`std::mt19937_64` and an open-interval uniform transform. A seed reproduces a
path with the same C++ standard-library implementation; distributions beyond
the raw engine may not be bit-identical across different library vendors.

The market adapter converts seconds to signed integer nanoseconds by flooring
`time_seconds * 1e9`. The configured horizon must be representable in that
timestamp range. Distinct Hawkes events may quantize to the same nanosecond;
the matching engine receives them in generated order.

## Prices, size, and time

Prices are integer ticks, quantities are integer lots, and exchange timestamps
are integer nanoseconds. A tick's currency value and a lot's share multiplier
are external instrument metadata, not inferred by matching. The simulator's
Hawkes baseline/decay units are seconds, independent of exchange timestamp
storage.

The adapter's price-placement, size, cancellation-target, and initial-book
rules are independent of Hawkes timing/type parameters. They are documented in
[exchange_rules.md](exchange_rules.md) and in the generated run manifest.
