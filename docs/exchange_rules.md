# Exchange rules and invariants

## Order book

The exchange is one continuous, single-instrument limit order book.

- Prices are signed 64-bit integer ticks; `price_ticks > 0`.
- Quantities are unsigned 64-bit integer lots; submitted quantity is positive.
- Bids are sorted highest price first; asks are sorted lowest price first.
- Each price level is FIFO in accepted insertion order.
- A buy trades against the lowest ask while its limit price is at least that
  ask; a sell trades against the highest bid while its limit is no greater than
  that bid.
- Trades execute at the resting order's price.
- A partial fill reduces the resting quantity without changing its FIFO place.
- A fully filled resting order is removed from the level and order-ID index.
- An unfilled limit residual rests at its limit price.
- An unfilled market residual expires immediately; it never rests.
- Cancellation removes the entire remaining quantity of a live order. Unknown
  IDs are rejected; cancel/replace is not quantity-partial.
- Accepted order IDs cannot be reused after fill, cancellation, or expiry.
  Invalid submissions do not reserve an ID.
- Invocation order defines exchange order. The exchange does not reorder calls
  by timestamp.

Because crossing limits execute before resting, a valid engine submission
cannot leave a locked/crossed book. There is no hidden liquidity, auction,
time-in-force policy, participant/self-trade identity, latency, or exchange
fee in the core matching engine.

## Book accounting and invariants

Every live order appears once in its price-level FIFO and once in the order-ID
index. The displayed quantity at a level equals the sum of remaining quantities
in that level's FIFO. A full test-only invariant checker verifies both sides,
best prices, index/queue agreement, aggregate depth, order lifecycle, and
contiguous trade IDs. Price-level quantity addition is checked for unsigned
overflow before mutation.

Each match emits a fill with trade ID, timestamp, resting price, quantity,
aggressor side, resting/incoming IDs, and post-fill remaining quantities. No
trade is created for a resting or cancelled order.

## Cancellation behavior

`MatchingEngine::cancel` accepts a nonzero ID and valid timestamp. A live order
returns a `Cancelled` record with its remaining quantity and is removed from
book depth. Repeated or unknown cancellation returns `UnknownOrderId`. The
Hawkes adapter's default cancellation policy selects the FIFO-front order at
the best price on the chosen side; the current `UniformRandom` policy is also
restricted to the best level, not all levels.

## Accounting boundaries

The exchange records executions, not a participant's cash/position. Strategy
cash, inventory, realized/unrealized P&L, fees, and risk reservations live in
the separate broker/evaluator. That accounting updates only from fills and
observable marks; an unfilled order is never counted as a trade. See
[strategy.md](strategy.md).

## Deterministic reference validation

The tests include hand-calculated priority/partial-fill cases and randomized
command sequences compared after every operation against a deliberately simple
vector-based reference book. The reference checks trades, live orders, depth,
best prices, and cancellations. It is a small-case oracle, not a proof over all
possible inputs.
