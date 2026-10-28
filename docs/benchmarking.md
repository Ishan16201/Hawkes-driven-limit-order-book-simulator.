# Benchmark methodology and measured results

## Reproduction command

```sh
cmake --preset release
cmake --build --preset release --parallel 4
./build/release/lob_benchmarks
```

The harness runs each workload five times and reports the median steady-clock
duration. Workload setup is outside the timer except for full simulation.
`events/ops` is the event or operation count used in the throughput calculation.
The best-price operation is one bid/ask lookup pair. Event-time throughput uses
the number of generated events; its simulated-time ratio is horizon divided by
wall time. These are local regression measurements, not portable performance
claims.

## Environment

Measurements were taken on macOS 27.0.1, arm64, Apple M5, Apple clang 21
(`-O3 -DNDEBUG`), CMake 4.4.2, C++20. No separate hardware-counter profiler was
available. Peak resident memory is from `/usr/bin/time -l` around the complete
benchmark process, not per workload.

## Release run

The current optimized matching measurements and the other local workloads:

| Workload | Operations/events | Median elapsed | Throughput / additional output |
|---|---:|---:|---|
| Best bid/ask lookup pair | 1,000,000 pairs | 1,871,917 ns | 534.2 million pairs/s |
| Order insertion | 20,000 distinct price levels | 1,484,542 ns | 13.47 million orders/s |
| Order cancellation | 20,000 preloaded orders | 833,166 ns | 24.00 million cancels/s |
| Matching | 1,000 resting asks swept by one market buy | 35,750 ns | 27.97 million fills/s |
| Hawkes update + intensity query | 100,000 alternating events | 2,864,291 ns | 34.91 million updates/s |
| Hawkes event-time simulation | 1,542 events, 1,000 simulated seconds | 161,583 ns | 9.54 million events/s; 6.19 million simulated seconds/wall second |
| NDJSON event-log serialization | 20,000 dispatches | 6,732,083 ns | 2.97 million records/s; 4,735,565 bytes |
| Full deterministic simulation | 20,000 limit submissions | 1,953,625 ns | 10.24 million events/s; 10.24 simulated seconds/wall second |

The full-simulation workload places 20,000 non-crossing buy orders at a fixed
price. It is not representative of a mixed-flow market. The Hawkes event-time
workload excludes mark generation, the exchange adapter, snapshots, and
analytics. The intensity-update case includes an intensity query/allocation
after each update.

## Benchmark-guided matching change

The first version of the expanded benchmark measured the same 1,000-order
market sweep at 1,046,750 ns median. Inspection of the matching loop showed it
copied the entire price-level FIFO into a vector on each fill. A direct
front-of-queue accessor removed that repeated allocation/copy. The final
matching workload measured 35,750 ns median, about 29.3x faster for this
particular queue sweep. Matching, FIFO, partial fills, and randomized
reference-book tests were rerun after the change. No other data structure was
changed based on unmeasured intuition.

The pre-existing benchmark had only measured one million
`branching_upper_bound` calls (8,048,666 ns in an earlier Release run); it did
not cover exchange or simulation throughput and is not included in the table.

## Memory and limitations

The entire benchmark process reached 36,388,864 bytes maximum resident size in
the recorded run. This does not characterize long-run memory growth. Matching
engine trades/events, Hawkes adapter events/trades/snapshots, and serialization
strings remain retained in memory; no bounded/streaming output mode exists.
The adapter's per-event full-book snapshots can dominate at larger depths.

Each timing is a five-run median from one machine and one compiler build, with
no confidence interval or allocation instrumentation. Repeat on the target
machine and retain raw output before making performance claims. The benchmark
does not yet cover the full Hawkes-market-adapter/analytics pipeline, parser
throughput, or peak memory as a function of simulated duration.
