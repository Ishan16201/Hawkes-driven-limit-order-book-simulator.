# Deterministic matching engine

`MatchingEngine` is independent of stochastic generation. A buy consumes the
lowest ask and a sell consumes the highest bid while executable; all trades use
the resting price and FIFO head at each level. Limit residuals rest. Market
residuals explicitly expire, are reported as `Expired`, and never rest.

The engine keeps accepted IDs so filled, cancelled, and expired IDs cannot be
reused; invalid submissions do not reserve IDs. It has no participant identity,
so self-trade prevention is out of scope. Valid submissions cannot create locked
or crossed resting books because crossing limits execute before residual insert.
No inconsistent state is silently repaired.

Every submission, execution, and cancellation appends an `ExchangeEvent`.
`Trade` records timestamp, resting price, quantity, aggressor, IDs, and
post-execution residual quantities. Invocation order, rather than timestamp
sorting, defines deterministic event order.
