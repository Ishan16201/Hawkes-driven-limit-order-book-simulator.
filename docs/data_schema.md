# Data and output schema

## Canonical replay command

`lob::simulation::CanonicalEvent` has these fields:

| Field | C++ type | Meaning |
|---|---|---|
| `event_id` | `uint64_t` | Nonzero, unique event identifier |
| `timestamp_ns` | signed `int64_t` | Nonnegative exchange timestamp in nanoseconds |
| `sequence` | `uint64_t` | Unique deterministic ordering key, including equal timestamps |
| `type` | enum | `Limit`, `Market`, or `Cancel` |
| `order_id` | `uint64_t` | Nonzero exchange command ID / cancellation target |
| `side` | enum | `Buy` or `Sell` |
| `price_ticks` | signed `int64_t` | Positive for limit; zero for market/cancel |
| `quantity` | `uint64_t` | Positive for limit/market; zero for cancel |

`Simulator::run` sorts by `(timestamp_ns, sequence)` and validates event IDs,
sequences, types, and command-specific fields. This is a command stream, not a
complete exchange-native market-data format.

The simulator replay log is one JSON object per dispatch, with `schema_version`,
event ID/time/sequence, numeric `command_type` plus `command_type_name`, order
ID, side, ticks/lots, acceptance, numeric rejection code, and number of fills
created by the command. Detailed fills are available from the `SimulationResult`
trade vector; the Hawkes executable writes a dedicated `trades.ndjson`.

## Historical input

`EventParser::parse_csv` expects a header in this order:

```text
version,timestamp_ns,source_sequence,event_type,side,price_ticks,quantity_lots,order_id,trade_id
```

`parse_ndjson` accepts one raw record per line with corresponding named fields.
The parser maps submit/add, execute/trade, cancel/delete, replace/modify, and
feed-status records into the six Hawkes event types. Replacements can be
decomposed into a cancellation and submission; repeated trade IDs can be
deduplicated.

The implementation is a basic parser, not a venue-certified feed adapter:
CSV quoting/escaped commas are not implemented, optional fields vary by event,
and raw venue semantics must be checked before interpreting mapped events.
The included [deterministic replay fixture](../fixtures/deterministic_replay.csv)
is synthetic and is used only to test parsing, mapping, partial fills, and a
cancellation.

## Hawkes simulation outputs

The simulator CLI can write four outputs:

```sh
./build/release/lob_simulator \
  --events-out build/release/events.ndjson \
  --trades-out build/release/trades.ndjson \
  --manifest-out build/release/run_manifest.json \
  --analytics-out build/release/analytics.json
```

`events.ndjson` has one record per Hawkes event: record/schema type, event time
in seconds, floored `timestamp_ns`, six-type label, outcome, order/cancel ID,
price ticks, quantity lots, trade count, and pre-event intensity vector.
`trades.ndjson` has one record per exchange fill with trade/order IDs, time,
resting price, quantity, aggressor side, and remaining quantities.
`run_manifest.json` records simulation seed, full Hawkes parameters, mark and
initial-book settings, event/trade counts, units, and empirical-validation
status. `analytics.json` contains the computed report.

All files are deterministic for the same configuration, seed, executable, and
standard-library RNG implementation. Hawkes event times are double seconds;
`timestamp_ns` is their floored integer conversion. Floating-point event times
and integer exchange times are both retained to make quantization visible.

## Units and provenance required for external data

Historical prices/lots are not scaled by the parser. A dataset must declare
instrument, tick value, lot multiplier, timestamp epoch and precision, source
sequence semantics, venue, event mapping, identifier scope, missing-field
rules, and provenance outside this parser. Raw data and normalized outputs
should be kept separate. No real venue dataset is included in this repository.
