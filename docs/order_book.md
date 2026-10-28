# Core order-book storage

`lob::exchange::OrderBook` is a storage-only component. It does not cross
orders, generate trades, apply fills, or model market flow.

The implementation uses a descending `std::map` for bid levels, an ascending
`std::map` for ask levels, a FIFO `std::list<OrderId>` at each level, and an
`std::unordered_map<OrderId, IndexedOrder>` lookup index. This supports
best-price access through `map::begin`, logarithmic level lookup, and direct
order cancellation through an indexed list iterator. The book owns all orders
and price levels by value; it exposes no raw owning pointers.

A binary heap is efficient for best price but unsuitable for arbitrary cancel:
removing an order or empty price level requires an additional index and heap
repair. A flat sorted vector can improve cache locality for small books, but
insertion/removal shifts levels and invalidates positions. Both remain possible
benchmark-informed alternatives. The ordered-map design is the simplest one that
preserves the required cancellation and ordering invariants without a separate
optimization layer.

`std::map` node references and iterators remain valid when other levels are
inserted or erased. `std::list` iterators remain valid except for the erased list
element. The ID index stores only the iterator to its own live queue node; it is
erased before the containing empty price level is erased. The book is deliberately
non-copyable and non-movable so no stored queue iterator can accidentally be
copied into a different container.

Prices are positive signed integer ticks and quantities are positive unsigned
integer lots. A zero order ID, invalid side, nonpositive price, zero quantity,
inconsistent new-order quantity, negative timestamp, duplicate live ID,
non-limit order, or non-`New` insertion status is rejected with
`std::invalid_argument`. Removal returns a `Cancelled` value record
and erases the live index entry; terminal order history belongs to a later event
log, avoiding duplicate mutable ownership in this structure.

When tests are built, `LOB_ENABLE_TESTING` exposes
`assert_invariants_for_testing()`. It verifies every queue/index relationship,
aggregate displayed quantity, side/price agreement, nonempty stored levels, and
that no index entry is absent from all price levels.
